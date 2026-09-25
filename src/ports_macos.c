/* macOS backend: walks every process's file descriptors through libproc. */
#ifdef __APPLE__
/* The Makefile asks for strict POSIX, which hides BSD types (u_short, u_int)
 * that libproc's own headers rely on. Darwin extensions bring them back. */
#define _DARWIN_C_SOURCE
#include "whoport.h"

#include <arpa/inet.h>
#include <libproc.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>

/* KERN_PROCARGS2 layout: int argc, exec path, NUL padding, argv[0..argc-1], env... */
static void read_command(int pid, char *out, size_t size) {
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    int argmax_mib[2] = {CTL_KERN, KERN_ARGMAX};
    int argmax = 0;
    size_t len = sizeof argmax;
    out[0] = '\0';
    if (sysctl(argmax_mib, 2, &argmax, &len, NULL, 0) != 0 || argmax <= 0) return;
    char *buf = malloc((size_t)argmax);
    if (!buf) return;
    len = (size_t)argmax;
    if (sysctl(mib, 3, buf, &len, NULL, 0) != 0 || len < sizeof(int)) {
        free(buf);
        return;
    }
    int argc;
    memcpy(&argc, buf, sizeof argc);
    char *p = buf + sizeof argc;
    char *end = buf + len;
    while (p < end && *p) p++; /* exec path */
    while (p < end && !*p) p++; /* padding */
    size_t w = 0;
    for (int i = 0; i < argc && p < end; i++) {
        size_t n = strnlen(p, (size_t)(end - p));
        if (w && w + 1 < size) out[w++] = ' ';
        for (size_t k = 0; k < n && w + 1 < size; k++) out[w++] = p[k];
        p += n + 1;
    }
    out[w] = '\0';
    free(buf);
}

static void fill_process(listener_t *l) {
    struct proc_bsdinfo bsd;
    if (proc_pidinfo(l->pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof bsd) == (int)sizeof bsd) {
        l->uid = bsd.pbi_uid;
        l->started = (long long)bsd.pbi_start_tvsec;
        wp_copy(l->name, sizeof l->name, bsd.pbi_name[0] ? bsd.pbi_name : bsd.pbi_comm);
    }
    struct proc_taskinfo task;
    if (proc_pidinfo(l->pid, PROC_PIDTASKINFO, 0, &task, sizeof task) == (int)sizeof task)
        l->rss = (long long)task.pti_resident_size;
    struct proc_vnodepathinfo vpi;
    if (proc_pidinfo(l->pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof vpi) == (int)sizeof vpi)
        wp_copy(l->cwd, sizeof l->cwd, vpi.pvi_cdir.vip_path);
    read_command(l->pid, l->command, sizeof l->command);
    if (!l->command[0]) wp_copy(l->command, sizeof l->command, l->name);
}

static void add_socket(listener_list *out, int pid, const struct socket_fdinfo *si) {
    const struct in_sockinfo *in = &si->psi.soi_proto.pri_tcp.tcpsi_ini;
    listener_t *l = wp_list_push(out);
    if (!l) return;
    l->pid = pid;
    l->port = ntohs((uint16_t)in->insi_lport);
    l->ipv6 = si->psi.soi_family == AF_INET6;
    char buf[INET6_ADDRSTRLEN] = "?";
    if (l->ipv6) inet_ntop(AF_INET6, &in->insi_laddr.ina_6, buf, sizeof buf);
    else inet_ntop(AF_INET, &in->insi_laddr.ina_46.i46a_addr4, buf, sizeof buf);
    if (strncmp(buf, "::ffff:", 7) == 0 && strchr(buf + 7, '.')) memmove(buf, buf + 7, strlen(buf + 7) + 1);
    snprintf(l->addr, sizeof l->addr, "%s", buf);
}

static int debug = -1;
#define DBG(...) do { if (debug) fprintf(stderr, "whoport debug: " __VA_ARGS__); } while (0)

int wp_collect(listener_list *out) {
    if (debug < 0) debug = getenv("WHOPORT_DEBUG") != NULL;
    int count = proc_listallpids(NULL, 0);
    DBG("proc_listallpids(NULL) = %d\n", count);
    if (count <= 0) return -1;
    int *pids = calloc((size_t)count + 64, sizeof *pids);
    if (!pids) return -1;
    count = proc_listallpids(pids, (int)((size_t)(count + 64) * sizeof *pids));
    DBG("proc_listallpids(buf) = %d\n", count);
    int with_fds = 0, sockets = 0, tcp = 0;

    for (int i = 0; i < count; i++) {
        int pid = pids[i];
        if (pid <= 0) continue;
        int size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
        if (size <= 0) continue;
        struct proc_fdinfo *fds = malloc((size_t)size);
        if (!fds) continue;
        size = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, size);
        int nfds = size > 0 ? size / (int)PROC_PIDLISTFD_SIZE : 0;
        with_fds++;
        size_t before = out->len;
        for (int f = 0; f < nfds; f++) {
            if (fds[f].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            sockets++;
            struct socket_fdinfo si;
            int got = proc_pidfdinfo(pid, fds[f].proc_fd, PROC_PIDFDSOCKETINFO, &si, PROC_PIDFDSOCKETINFO_SIZE);
            if (got != PROC_PIDFDSOCKETINFO_SIZE) {
                DBG("pid %d fd %d: proc_pidfdinfo returned %d (want %d)\n", pid, fds[f].proc_fd, got,
                    (int)PROC_PIDFDSOCKETINFO_SIZE);
                continue;
            }
            DBG("pid %d fd %d: family %d kind %d state %d lport %d\n", pid, fds[f].proc_fd, si.psi.soi_family,
                si.psi.soi_kind, si.psi.soi_proto.pri_tcp.tcpsi_state,
                ntohs((uint16_t)si.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport));
            if (si.psi.soi_kind != SOCKINFO_TCP) continue;
            tcp++;
            if (si.psi.soi_proto.pri_tcp.tcpsi_state != TSI_S_LISTEN) continue;
            add_socket(out, pid, &si);
        }
        free(fds);
        if (out->len > before) {
            listener_t info;
            memset(&info, 0, sizeof info);
            info.pid = pid;
            info.started = -1;
            info.rss = -1;
            fill_process(&info);
            for (size_t k = before; k < out->len; k++) {
                listener_t *l = &out->items[k];
                l->uid = info.uid;
                l->started = info.started;
                l->rss = info.rss;
                memcpy(l->name, info.name, sizeof l->name);
                memcpy(l->command, info.command, sizeof l->command);
                memcpy(l->cwd, info.cwd, sizeof l->cwd);
            }
        }
    }
    free(pids);
    DBG("processes with fds: %d, sockets: %d, tcp: %d, listening: %zu\n", with_fds, sockets, tcp, out->len);
    wp_list_sort_dedupe(out);
    return 0;
}
#else
typedef int wp_macos_backend_unused;
#endif
