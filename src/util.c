/* Platform-independent helpers: list handling, formatting, project detection. */
#define _DEFAULT_SOURCE
#include "whoport.h"

#ifndef _WIN32
#include <arpa/inet.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Copy with truncation. Long command lines and names are cut on purpose. */
void wp_copy(char *dst, size_t size, const char *src) {
    if (!size) return;
    size_t n = strlen(src);
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

listener_t *wp_list_push(listener_list *list) {
    if (list->len == list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 32;
        listener_t *items = realloc(list->items, cap * sizeof *items);
        if (!items) return NULL;
        list->items = items;
        list->cap = cap;
    }
    listener_t *l = &list->items[list->len++];
    memset(l, 0, sizeof *l);
    l->pid = -1;
    l->started = -1;
    l->rss = -1;
    return l;
}

void wp_list_free(listener_list *list) {
    free(list->items);
    list->items = NULL;
    list->len = list->cap = 0;
}

static int is_wildcard(const char *addr) {
    return strcmp(addr, "0.0.0.0") == 0 || strcmp(addr, "::") == 0 || strcmp(addr, "*") == 0;
}

int wp_is_loopback(const char *addr) {
    return strncmp(addr, "127.", 4) == 0 || strcmp(addr, "::1") == 0 || strcmp(addr, "localhost") == 0;
}

static int cmp_listener(const void *a, const void *b) {
    const listener_t *x = a, *y = b;
    if (x->port != y->port) return x->port - y->port;
    if (x->pid != y->pid) return x->pid - y->pid;
    return x->ipv6 - y->ipv6;
}

/* Sort by port and merge the IPv4 and IPv6 sockets a server usually opens on
 * the same port into one entry. */
void wp_list_sort_dedupe(listener_list *list) {
    if (list->len < 2) return;
    qsort(list->items, list->len, sizeof *list->items, cmp_listener);
    size_t w = 0;
    for (size_t r = 0; r < list->len; r++) {
        listener_t *cur = &list->items[r];
        if (w > 0) {
            listener_t *prev = &list->items[w - 1];
            if (prev->port == cur->port && prev->pid == cur->pid) {
                if (is_wildcard(cur->addr) && !is_wildcard(prev->addr)) {
                    wp_copy(prev->addr, sizeof prev->addr, cur->addr);
                } else if (wp_is_loopback(prev->addr) && wp_is_loopback(cur->addr) &&
                           strcmp(prev->addr, cur->addr) != 0) {
                    wp_copy(prev->addr, sizeof prev->addr, "localhost");
                }
                continue;
            }
        }
        if (w != r) list->items[w] = *cur;
        w++;
    }
    list->len = w;
}

void wp_format_duration(long long s, char *out, size_t size) {
    if (s < 0) snprintf(out, size, "?");
    else if (s < 60) snprintf(out, size, "%llds", s);
    else if (s < 3600) snprintf(out, size, "%lldm", s / 60);
    else if (s < 86400) snprintf(out, size, "%lldh %lldm", s / 3600, (s % 3600) / 60);
    else snprintf(out, size, "%lldd %lldh", s / 86400, (s % 86400) / 3600);
}

void wp_format_bytes(long long b, char *out, size_t size) {
    const double mb = 1024.0 * 1024.0;
    if (b < 0) snprintf(out, size, "?");
    else if (b < 1024 * 1024) snprintf(out, size, "%lld KB", (b + 1023) / 1024);
    else if (b < 1024LL * 1024 * 1024) snprintf(out, size, "%.0f MB", (double)b / mb);
    else snprintf(out, size, "%.1f GB", (double)b / (mb * 1024.0));
}

void wp_shorten_home(const char *path, const char *home, char *out, size_t size) {
    size_t n = home ? strlen(home) : 0;
    if (n > 1 && strncmp(path, home, n) == 0 && (path[n] == '/' || path[n] == '\0')) {
        snprintf(out, size, "~%s", path + n);
    } else {
        snprintf(out, size, "%s", path);
    }
}

static const char *const MARKERS[] = {
    ".git", "package.json", "Cargo.toml", "go.mod", "pyproject.toml", "requirements.txt",
    "Gemfile", "composer.json", "pom.xml", "build.gradle", "build.gradle.kts", "deno.json",
    "mix.exs", "Package.swift", "CMakeLists.txt", "Makefile", "docker-compose.yml", "compose.yaml",
    NULL,
};

static int has_marker(const char *dir) {
    char path[WP_PATH_MAX + 64];
    struct stat st;
    for (int i = 0; MARKERS[i]; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, MARKERS[i]);
        if (stat(path, &st) == 0) return 1;
    }
    return 0;
}

/* Walk up from `dir` to the nearest folder that looks like a project root.
 * Never climbs above the home directory. Writes `dir` itself when nothing is
 * found and returns 1 when a project root was found. */
int wp_find_project_root(const char *dir, const char *home, char *out, size_t size) {
    char cur[WP_PATH_MAX];
    snprintf(out, size, "%s", dir);
    if (!dir[0] || strcmp(dir, "/") == 0) return 0;
    snprintf(cur, sizeof cur, "%s", dir);
    for (;;) {
        if (has_marker(cur)) {
            snprintf(out, size, "%s", cur);
            return 1;
        }
        if (home && strcmp(cur, home) == 0) return 0;
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur) return 0;
        if (slash == cur + 2 && cur[1] == ':') return 0; /* "C:/" on Windows */
        *slash = '\0';
    }
}

/* "node /home/me/app/node_modules/.bin/vite --port 5173" -> "node vite --port 5173" */
void wp_short_command(const char *command, const char *home, char *out, size_t size) {
    (void)home;
    size_t w = 0;
    const char *p = command;
    out[0] = '\0';
    while (*p && w + 1 < size) {
        while (*p == ' ') p++;
        if (!*p) break;
        const char *start = p;
        size_t len;
        if (*p == '"') { /* Windows quotes paths with spaces: "C:\\Program Files\\node.exe" */
            start = ++p;
            while (*p && *p != '"') p++;
            len = (size_t)(p - start);
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ') p++;
            len = (size_t)(p - start);
        }
        const char *end = start + len;
        const char *tok = start;
        /* Paths shrink to their last component. */
        if (len > 1 && (memchr(start, '/', len) || memchr(start, '\\', len))) {
            const char *base = start;
            for (const char *q = start; q < end; q++)
                if ((*q == '/' || *q == '\\') && q + 1 < end) base = q + 1;
            len -= (size_t)(base - start);
            tok = base;
        }
        /* "node.exe" reads as "node". */
        if (len > 4 && strncmp(tok + len - 4, ".exe", 4) == 0) len -= 4;
        if (w > 0 && w + 1 < size) out[w++] = ' ';
        for (size_t i = 0; i < len && w + 1 < size; i++) out[w++] = tok[i];
        out[w] = '\0';
    }
}

int wp_parse_port(const char *s) {
    if (!s || !*s) return -1;
    long v = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return -1;
        v = v * 10 + (*p - '0');
        if (v > 65535) return -1;
    }
    return v >= 1 ? (int)v : -1;
}

#ifndef _WIN32
/* One line of /proc/net/tcp or /proc/net/tcp6. Addresses are hex words in
 * host (little-endian) byte order. Returns 1 on success. */
int wp_parse_proc_net_line(const char *line, int ipv6, int *port, char *addr, size_t addr_size,
                           unsigned long *inode, int *state) {
    char hex[40];
    unsigned int p, st;
    unsigned long ino;
    if (sscanf(line, " %*d: %39[0-9A-Fa-f]:%x %*s %x %*s %*s %*s %*u %*d %lu", hex, &p, &st, &ino) != 4)
        return 0;
    unsigned char bytes[16];
    size_t words = ipv6 ? 4 : 1;
    if (strlen(hex) != words * 8) return 0;
    for (size_t w = 0; w < words; w++) {
        char part[9];
        memcpy(part, hex + w * 8, 8);
        part[8] = '\0';
        unsigned long v = strtoul(part, NULL, 16);
        for (int b = 0; b < 4; b++) bytes[w * 4 + b] = (unsigned char)((v >> (8 * b)) & 0xff);
    }
    char buf[INET6_ADDRSTRLEN];
    if (!inet_ntop(ipv6 ? AF_INET6 : AF_INET, bytes, buf, sizeof buf)) return 0;
    /* IPv4-mapped IPv6 addresses read better as plain IPv4. */
    if (ipv6 && strncmp(buf, "::ffff:", 7) == 0 && strchr(buf + 7, '.'))
        memmove(buf, buf + 7, strlen(buf + 7) + 1);
    snprintf(addr, addr_size, "%s", buf);
    *port = (int)p;
    *inode = ino;
    *state = (int)st;
    return 1;
}
#endif
