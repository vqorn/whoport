/* Windows backend: listening sockets from GetExtendedTcpTable, details from
 * the process itself. The working directory is not exposed by any public API,
 * so it is read from the process parameters block in the target's PEB. */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0602 /* Windows 8: ProcessCommandLineInformation */
#endif
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "whoport.h"

#define PROCESS_COMMAND_LINE_INFORMATION ((PROCESSINFOCLASS)60)
#define FILETIME_UNIX_EPOCH 116444736000000000ULL

static int vt_enabled = 0;

void wp_platform_init(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleOutputCP(CP_UTF8);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (out != INVALID_HANDLE_VALUE && GetConsoleMode(out, &mode) &&
        SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
        vt_enabled = 1;
}

int wp_stdout_is_tty(void) {
    /* Colours need a console that understands escape codes. */
    return _isatty(_fileno(stdout)) && vt_enabled;
}

static void to_utf8(const WCHAR *w, size_t wlen, char *out, size_t size) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, (int)wlen, out, (int)size - 1, NULL, NULL);
    out[n > 0 ? n : 0] = '\0';
}

static void forward_slashes(char *s) {
    for (; *s; s++)
        if (*s == '\\') *s = '/';
}

const char *wp_home(void) {
    static char home[WP_PATH_MAX];
    if (!home[0]) {
        const WCHAR *w = _wgetenv(L"USERPROFILE");
        if (!w) return NULL;
        to_utf8(w, wcslen(w), home, sizeof home);
        forward_slashes(home);
    }
    return home;
}

int wp_is_alive(int pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    int alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

/* Windows has no SIGTERM for console programs, so both modes terminate. */
int wp_terminate(int pid, int force, char *err, size_t err_size) {
    (void)force;
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, (DWORD)pid);
    if (h && TerminateProcess(h, 1)) {
        CloseHandle(h);
        return 0;
    }
    DWORD e = GetLastError();
    if (h) CloseHandle(h);
    if (e == ERROR_ACCESS_DENIED) snprintf(err, err_size, "access denied, try an administrator terminal");
    else snprintf(err, err_size, "error %lu", (unsigned long)e);
    return -1;
}

void wp_sleep_ms(int ms) {
    Sleep((DWORD)ms);
}

void wp_localtime(long long t, struct tm *out) {
    time_t tt = (time_t)t;
    localtime_s(out, &tt);
}

/* ---------- Process details ---------- */

typedef struct {
    DWORD pid;
    char name[64];
} proc_name;

static proc_name *names = NULL;
static size_t n_names = 0;

static void load_names(void) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe;
    pe.dwSize = sizeof pe;
    size_t cap = 0;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (n_names == cap) {
            cap = cap ? cap * 2 : 256;
            proc_name *grown = realloc(names, cap * sizeof *grown);
            if (!grown) break;
            names = grown;
        }
        names[n_names].pid = pe.th32ProcessID;
        to_utf8(pe.szExeFile, wcslen(pe.szExeFile), names[n_names].name, sizeof names[n_names].name);
        n_names++;
    }
    CloseHandle(snap);
}

static const char *name_of(DWORD pid) {
    for (size_t i = 0; i < n_names; i++)
        if (names[i].pid == pid) return names[i].name;
    return NULL;
}

static void strip_exe(char *name) {
    size_t n = strlen(name);
    if (n > 4 && _stricmp(name + n - 4, ".exe") == 0) name[n - 4] = '\0';
}

static void read_command_line(HANDLE h, listener_t *l) {
    ULONG size = 0;
    NtQueryInformationProcess(h, PROCESS_COMMAND_LINE_INFORMATION, NULL, 0, &size);
    if (size == 0 || size > 1 << 20) return;
    void *buf = malloc(size);
    if (!buf) return;
    if (NT_SUCCESS(NtQueryInformationProcess(h, PROCESS_COMMAND_LINE_INFORMATION, buf, size, &size))) {
        UNICODE_STRING *us = buf;
        if (us->Buffer && us->Length) to_utf8(us->Buffer, us->Length / sizeof(WCHAR), l->command, sizeof l->command);
    }
    free(buf);
}

#ifdef _WIN64
/* Offsets in the 64-bit PEB and RTL_USER_PROCESS_PARAMETERS. Stable since
 * Windows XP x64: PEB.ProcessParameters at 0x20, CurrentDirectory.DosPath at 0x38. */
static void read_cwd(HANDLE h, listener_t *l) {
    PROCESS_BASIC_INFORMATION pbi;
    if (!NT_SUCCESS(NtQueryInformationProcess(h, ProcessBasicInformation, &pbi, sizeof pbi, NULL))) return;
    if (!pbi.PebBaseAddress) return;
    void *params = NULL;
    UNICODE_STRING dir;
    if (!ReadProcessMemory(h, (const char *)pbi.PebBaseAddress + 0x20, &params, sizeof params, NULL) || !params)
        return;
    if (!ReadProcessMemory(h, (const char *)params + 0x38, &dir, sizeof dir, NULL)) return;
    if (!dir.Buffer || dir.Length == 0 || dir.Length > 32767 * sizeof(WCHAR)) return;
    WCHAR *w = malloc(dir.Length);
    if (!w) return;
    if (ReadProcessMemory(h, dir.Buffer, w, dir.Length, NULL)) {
        to_utf8(w, dir.Length / sizeof(WCHAR), l->cwd, sizeof l->cwd);
        forward_slashes(l->cwd);
        size_t n = strlen(l->cwd);
        if (n > 3 && l->cwd[n - 1] == '/') l->cwd[n - 1] = '\0'; /* "C:/code/" -> "C:/code" */
    }
    free(w);
}
#else
static void read_cwd(HANDLE h, listener_t *l) {
    (void)h;
    (void)l;
}
#endif

static void fill_process(listener_t *l) {
    DWORD pid = (DWORD)l->pid;
    const char *snap_name = name_of(pid);
    if (snap_name) wp_copy(l->name, sizeof l->name, snap_name);
    if (pid == 0 || pid == 4) {
        wp_copy(l->name, sizeof l->name, "System");
        wp_copy(l->command, sizeof l->command, "System");
        return;
    }

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        strip_exe(l->name);
        wp_copy(l->command, sizeof l->command, l->name[0] ? l->name : "?");
        l->restricted = 1;
        return;
    }

    WCHAR image[MAX_PATH * 2];
    DWORD len = sizeof image / sizeof image[0];
    if (QueryFullProcessImageNameW(h, 0, image, &len)) {
        char path[WP_PATH_MAX];
        to_utf8(image, len, path, sizeof path);
        forward_slashes(path);
        const char *base = strrchr(path, '/');
        wp_copy(l->name, sizeof l->name, base ? base + 1 : path);
    }
    strip_exe(l->name);

    read_command_line(h, l);
    if (!l->command[0]) wp_copy(l->command, sizeof l->command, l->name);
    read_cwd(h, l);

    FILETIME created, exited, kernel, user;
    if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
        ULARGE_INTEGER t;
        t.LowPart = created.dwLowDateTime;
        t.HighPart = created.dwHighDateTime;
        if (t.QuadPart > FILETIME_UNIX_EPOCH) l->started = (long long)((t.QuadPart - FILETIME_UNIX_EPOCH) / 10000000ULL);
    }
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(h, &pmc, sizeof pmc)) l->rss = (long long)pmc.WorkingSetSize;
    CloseHandle(h);
}

/* ---------- Listening sockets ---------- */

static void *tcp_table(ULONG family) {
    DWORD size = 0;
    void *buf = NULL;
    DWORD r = ERROR_INSUFFICIENT_BUFFER;
    for (int tries = 0; tries < 5 && r == ERROR_INSUFFICIENT_BUFFER; tries++) {
        r = GetExtendedTcpTable(buf, &size, FALSE, family, TCP_TABLE_OWNER_PID_LISTENER, 0);
        if (r == ERROR_INSUFFICIENT_BUFFER) {
            free(buf);
            buf = malloc(size);
            if (!buf) return NULL;
        }
    }
    if (r != NO_ERROR) {
        free(buf);
        return NULL;
    }
    return buf;
}

int wp_collect(listener_list *out) {
    load_names();

    MIB_TCPTABLE_OWNER_PID *v4 = tcp_table(AF_INET);
    if (v4) {
        for (DWORD i = 0; i < v4->dwNumEntries; i++) {
            const MIB_TCPROW_OWNER_PID *row = &v4->table[i];
            listener_t *l = wp_list_push(out);
            if (!l) break;
            l->port = ntohs((u_short)row->dwLocalPort);
            l->pid = (int)row->dwOwningPid;
            struct in_addr a;
            a.S_un.S_addr = row->dwLocalAddr;
            inet_ntop(AF_INET, &a, l->addr, sizeof l->addr);
        }
        free(v4);
    }

    MIB_TCP6TABLE_OWNER_PID *v6 = tcp_table(AF_INET6);
    if (v6) {
        for (DWORD i = 0; i < v6->dwNumEntries; i++) {
            const MIB_TCP6ROW_OWNER_PID *row = &v6->table[i];
            listener_t *l = wp_list_push(out);
            if (!l) break;
            l->port = ntohs((u_short)row->dwLocalPort);
            l->pid = (int)row->dwOwningPid;
            l->ipv6 = 1;
            inet_ntop(AF_INET6, row->ucLocalAddr, l->addr, sizeof l->addr);
        }
        free(v6);
    }

    for (size_t i = 0; i < out->len; i++) fill_process(&out->items[i]);
    free(names);
    names = NULL;
    n_names = 0;
    wp_list_sort_dedupe(out);
    return 0;
}
#else
typedef int wp_windows_backend_unused;
#endif
