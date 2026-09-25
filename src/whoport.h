/* whoport: see which project is running on which port. */
#ifndef WHOPORT_H
#define WHOPORT_H

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

#define WP_CMD_MAX 512
#define WP_PATH_MAX 1024

typedef struct {
    int port;
    int ipv6;                  /* 1 if the socket is IPv6 */
    char addr[64];             /* bound address, e.g. "127.0.0.1", "0.0.0.0", "::" */
    unsigned long inode;       /* Linux socket inode, 0 elsewhere */
    int pid;                   /* -1 when the owning process is not visible */
    unsigned int uid;         /* owner (0 on Windows) */
    char name[64];             /* short process name, e.g. "node" */
    char command[WP_CMD_MAX];  /* full command line */
    char cwd[WP_PATH_MAX];     /* working directory of the process */
    long long started;         /* start time, seconds since the epoch, -1 if unknown */
    long long rss;             /* resident memory in bytes, -1 if unknown */
} listener_t;

typedef struct {
    listener_t *items;
    size_t len;
    size_t cap;
} listener_list;

/* Platform backends (ports_linux.c / ports_macos.c / ports_windows.c). Returns 0 on success. */
int wp_collect(listener_list *out);

/* Process control and environment (platform_posix.c / ports_windows.c). */
void wp_platform_init(void);
int wp_stdout_is_tty(void);
const char *wp_home(void);
int wp_is_alive(int pid);
int wp_terminate(int pid, int force, char *err, size_t err_size); /* 0 when the signal was sent */
void wp_sleep_ms(int ms);
void wp_localtime(long long t, struct tm *out);

/* util.c */
void wp_copy(char *dst, size_t size, const char *src);
listener_t *wp_list_push(listener_list *list);
void wp_list_free(listener_list *list);
void wp_list_sort_dedupe(listener_list *list);

void wp_format_duration(long long seconds, char *out, size_t size);
void wp_format_bytes(long long bytes, char *out, size_t size);
void wp_shorten_home(const char *path, const char *home, char *out, size_t size);
int wp_find_project_root(const char *dir, const char *home, char *out, size_t size);
void wp_short_command(const char *command, const char *home, char *out, size_t size);
int wp_parse_port(const char *s);
#ifndef _WIN32
int wp_parse_proc_net_line(const char *line, int ipv6, int *port, char *addr, size_t addr_size,
                           unsigned long *inode, int *state);
#endif
int wp_is_loopback(const char *addr);

#endif
