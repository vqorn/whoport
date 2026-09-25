/* Linux backend: listening sockets from /proc/net/tcp{,6}, owners from /proc/<pid>/fd. */
#ifdef __linux__
#define _DEFAULT_SOURCE
#include "whoport.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TCP_LISTEN 0x0A

static void read_listeners(const char *file, int ipv6, listener_list *out) {
    FILE *f = fopen(file, "r");
    if (!f) return;
    char line[512];
    if (!fgets(line, sizeof line, f)) { /* header */
        fclose(f);
        return;
    }
    while (fgets(line, sizeof line, f)) {
        int port, state;
        unsigned long inode;
        char addr[64];
        if (!wp_parse_proc_net_line(line, ipv6, &port, addr, sizeof addr, &inode, &state)) continue;
        if (state != TCP_LISTEN || inode == 0) continue;
        listener_t *l = wp_list_push(out);
        if (!l) break;
        l->port = port;
        l->ipv6 = ipv6;
        l->inode = inode;
        snprintf(l->addr, sizeof l->addr, "%s", addr);
    }
    fclose(f);
}

static ssize_t read_file(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (ssize_t)n;
}

static long long boot_time(void) {
    static long long btime = -2;
    if (btime != -2) return btime;
    btime = -1;
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return btime;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "btime %lld", &btime) == 1) break;
    }
    fclose(f);
    return btime;
}

static void fill_process(listener_t *l) {
    char path[64], buf[4096];
    int pid = l->pid;

    snprintf(path, sizeof path, "/proc/%d", pid);
    struct stat st;
    if (stat(path, &st) == 0) l->uid = st.st_uid;

    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    if (read_file(path, buf, sizeof buf) > 0) {
        buf[strcspn(buf, "\n")] = '\0';
        wp_copy(l->name, sizeof l->name, buf);
    }

    /* Arguments are NUL-separated. */
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    ssize_t n = read_file(path, buf, sizeof buf);
    if (n > 0) {
        for (ssize_t i = 0; i < n; i++)
            if (buf[i] == '\0') buf[i] = ' ';
        while (n > 0 && buf[n - 1] == ' ') buf[--n] = '\0';
        wp_copy(l->command, sizeof l->command, buf);
    }
    if (!l->command[0]) snprintf(l->command, sizeof l->command, "%s", l->name);

    snprintf(path, sizeof path, "/proc/%d/cwd", pid);
    ssize_t len = readlink(path, l->cwd, sizeof l->cwd - 1);
    l->cwd[len > 0 ? len : 0] = '\0';

    /* Field 22 of /proc/<pid>/stat is the start time in clock ticks after boot.
     * The command name (field 2) may contain spaces, so parse after the last ')'. */
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    if (read_file(path, buf, sizeof buf) > 0) {
        char *p = strrchr(buf, ')');
        unsigned long long start;
        if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                        &start) == 1) {
            long ticks = sysconf(_SC_CLK_TCK);
            long long bt = boot_time();
            if (ticks > 0 && bt > 0) l->started = bt + (long long)(start / (unsigned long long)ticks);
        }
    }

    snprintf(path, sizeof path, "/proc/%d/statm", pid);
    if (read_file(path, buf, sizeof buf) > 0) {
        long long pages;
        if (sscanf(buf, "%*d %lld", &pages) == 1) l->rss = pages * sysconf(_SC_PAGESIZE);
    }
}

/* Find which process holds each socket inode by scanning every readable
 * /proc/<pid>/fd. Processes of other users are skipped without root. */
static void match_owners(listener_list *list) {
    DIR *proc = opendir("/proc");
    if (!proc) return;
    size_t unmatched = list->len;
    struct dirent *de;
    while (unmatched > 0 && (de = readdir(proc))) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        int pid = atoi(de->d_name);
        char fddir[64];
        snprintf(fddir, sizeof fddir, "/proc/%d/fd", pid);
        DIR *fds = opendir(fddir);
        if (!fds) continue;
        struct dirent *fe;
        while ((fe = readdir(fds))) {
            if (fe->d_name[0] == '.') continue;
            char link[512], target[64];
            snprintf(link, sizeof link, "%s/%s", fddir, fe->d_name);
            ssize_t n = readlink(link, target, sizeof target - 1);
            if (n <= 0) continue;
            target[n] = '\0';
            unsigned long inode;
            if (sscanf(target, "socket:[%lu]", &inode) != 1) continue;
            for (size_t i = 0; i < list->len; i++) {
                if (list->items[i].inode == inode && list->items[i].pid < 0) {
                    list->items[i].pid = pid;
                    unmatched--;
                }
            }
        }
        closedir(fds);
    }
    closedir(proc);
}

int wp_collect(listener_list *out) {
    read_listeners("/proc/net/tcp", 0, out);
    read_listeners("/proc/net/tcp6", 1, out);
    match_owners(out);
    for (size_t i = 0; i < out->len; i++)
        if (out->items[i].pid > 0) fill_process(&out->items[i]);
    wp_list_sort_dedupe(out);
    return 0;
}
#else
typedef int wp_linux_backend_unused;
#endif
