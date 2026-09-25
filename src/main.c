/* whoport: see which project is running on which port, and stop it. */
#define _DEFAULT_SOURCE
#include "whoport.h"

#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef WHOPORT_VERSION
#define WHOPORT_VERSION "dev"
#endif

#define MAX_QUERY 64
#define LONG_RUNNING (24 * 3600)

static int color = 1;
static const char *home = NULL;

#define C(code) (color ? "\033[" code "m" : "")
#define RESET C("0")
#define BOLD C("1")
#define DIM C("2")
#define RED C("31")
#define GREEN C("32")
#define YELLOW C("33")
#define CYAN C("36")

static void usage(FILE *f) {
    fprintf(f,
            "whoport %s: see which project is running on which port.\n"
            "\n"
            "Usage\n"
            "  whoport                  list every listening port\n"
            "  whoport <port>...        show who is using these ports\n"
            "  whoport <port> --kill    stop the process on that port\n"
            "\n"
            "Options\n"
            "  -k, --kill      stop the process (SIGTERM, then waits up to 3 seconds)\n"
            "  -f, --force     with --kill: use SIGKILL if it does not stop in time\n"
            "  -j, --json      machine-readable output\n"
            "      --color     force colours, e.g. when piping into less -R\n"
            "      --no-color  disable colours (also: NO_COLOR=1)\n"
            "  -h, --help      show this help\n"
            "  -v, --version   show the version\n"
            "\n"
            "Exit status: 0 if a queried port is in use, 1 if it is free, 2 on errors.\n"
            "Processes of other users are only visible with sudo.\n",
            WHOPORT_VERSION);
}

/* Project folder of a listener, shortened for display. */
static void project_of(const listener_t *l, char *out, size_t size) {
    if (l->pid < 0) {
        snprintf(out, size, "?");
        return;
    }
    if (!l->cwd[0] || strcmp(l->cwd, "/") == 0) {
        snprintf(out, size, "(system)");
        return;
    }
    char root[WP_PATH_MAX];
    wp_find_project_root(l->cwd, home, root, sizeof root);
    wp_shorten_home(root, home, out, size);
}

static long long uptime_of(const listener_t *l, time_t now) {
    return l->started > 0 ? (long long)now - l->started : -1;
}

static void fit(const char *in, int width, char *out, size_t size) {
    int len = (int)strlen(in);
    if (len <= width) {
        snprintf(out, size, "%s", in);
    } else if (width > 3) {
        /* Keep the end of paths, the start of commands: both read better that way. */
        if (in[0] == '~' || in[0] == '/') snprintf(out, size, "...%s", in + len - (width - 3));
        else snprintf(out, size, "%.*s...", width - 3, in);
    } else {
        snprintf(out, size, "%.*s", width, in);
    }
}

static void json_string(const char *s) {
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') printf("\\%c", c);
        else if (c == '\n') printf("\\n");
        else if (c == '\t') printf("\\t");
        else if (c < 0x20) printf("\\u%04x", c);
        else putchar(c);
    }
    putchar('"');
}

static void print_json(const listener_list *list, time_t now) {
    printf("[");
    for (size_t i = 0; i < list->len; i++) {
        const listener_t *l = &list->items[i];
        char project[WP_PATH_MAX];
        if (l->pid >= 0 && l->cwd[0]) wp_find_project_root(l->cwd, home, project, sizeof project);
        else project[0] = '\0';
        printf("%s\n  {\"port\": %d, \"address\": ", i ? "," : "", l->port);
        json_string(l->addr);
        if (l->pid < 0) {
            printf(", \"pid\": null}");
            continue;
        }
        printf(", \"pid\": %d, \"name\": ", l->pid);
        json_string(l->name);
        printf(", \"command\": ");
        json_string(l->command);
        printf(", \"cwd\": ");
        json_string(l->cwd);
        printf(", \"project\": ");
        json_string(project);
        printf(", \"uptime_seconds\": %lld, \"memory_bytes\": %lld}", uptime_of(l, now), l->rss);
    }
    printf("%s]\n", list->len ? "\n" : "");
}

static void print_table(const listener_list *list, time_t now) {
    if (!list->len) {
        printf("No listening ports found.\n");
        return;
    }
    int w_proj = 7, w_cmd = 7;
    char proj[WP_PATH_MAX], cmd[WP_CMD_MAX], cell[WP_PATH_MAX];
    for (size_t i = 0; i < list->len; i++) {
        project_of(&list->items[i], proj, sizeof proj);
        wp_short_command(list->items[i].command, home, cmd, sizeof cmd);
        if ((int)strlen(proj) > w_proj) w_proj = (int)strlen(proj);
        if ((int)strlen(cmd) > w_cmd) w_cmd = (int)strlen(cmd);
    }
    if (w_proj > 30) w_proj = 30;
    if (w_cmd > 36) w_cmd = 36;

    printf("\n  %s%-6s %-*s  %-*s  %7s  %9s  %8s%s\n", DIM, "PORT", w_proj, "PROJECT", w_cmd, "COMMAND", "PID",
           "RUNNING", "MEMORY", RESET);
    int hidden = 0, stale = 0, stale_port = 0;
    for (size_t i = 0; i < list->len; i++) {
        const listener_t *l = &list->items[i];
        if (l->pid < 0) {
            hidden++;
            printf("  %s%-6d%s %s%-*s  %s%s\n", BOLD, l->port, RESET, DIM, w_proj, "?", "(another user, try sudo)",
                   RESET);
            continue;
        }
        char up[32], mem[32], pid[16];
        long long secs = uptime_of(l, now);
        wp_format_duration(secs, up, sizeof up);
        wp_format_bytes(l->rss, mem, sizeof mem);
        snprintf(pid, sizeof pid, "%d", l->pid);
        project_of(l, proj, sizeof proj);
        wp_short_command(l->command, home, cmd, sizeof cmd);

        int is_stale = secs >= LONG_RUNNING && home && strncmp(l->cwd, home, strlen(home)) == 0;
        if (is_stale) {
            stale++;
            stale_port = l->port;
        }
        printf("  %s%-6d%s ", BOLD, l->port, RESET);
        fit(proj, w_proj, cell, sizeof cell);
        printf("%s%-*s%s  ", CYAN, w_proj, cell, RESET);
        fit(cmd, w_cmd, cell, sizeof cell);
        printf("%-*s  %s%7s%s  %s%9s%s  %8s", w_cmd, cell, DIM, pid, RESET, is_stale ? YELLOW : "", up, RESET, mem);
        if (is_stale) printf("  %s<- forgotten?%s", YELLOW, RESET);
        printf("\n");
    }
    printf("\n");
    if (stale == 1) printf("  %sRunning for more than a day. Stop it with: whoport %d --kill%s\n\n", DIM, stale_port, RESET);
    else if (stale > 1) printf("  %s%d servers have been running for more than a day.%s\n\n", DIM, stale, RESET);
    if (hidden) printf("  %s%d port%s owned by other users. Run with sudo to see them.%s\n\n", DIM, hidden,
                       hidden == 1 ? " is" : "s are", RESET);
}

static void print_detail(const listener_t *l, time_t now) {
    char project[WP_PATH_MAX], up[32], mem[32], when[32];
    if (l->pid < 0) {
        printf("\n  %sPort %d%s is in use by a process of another user.\n  Run %ssudo whoport %d%s to see it.\n\n",
               BOLD, l->port, RESET, BOLD, l->port, RESET);
        return;
    }
    project_of(l, project, sizeof project);
    wp_format_duration(uptime_of(l, now), up, sizeof up);
    wp_format_bytes(l->rss, mem, sizeof mem);
    when[0] = '\0';
    if (l->started > 0) {
        time_t t = (time_t)l->started;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof when, now - t < 86400 ? "since %H:%M" : "since %b %d, %H:%M", &tm);
    }
    printf("\n  %sPort %d%s is used by %s%s%s %s(pid %d)%s\n\n", BOLD, l->port, RESET, BOLD, l->name, RESET, DIM,
           l->pid, RESET);
    printf("  %-9s %s%s%s\n", "Project", CYAN, project, RESET);
    printf("  %-9s %s\n", "Command", l->command);
    printf("  %-9s %s %s%s%s\n", "Running", up, DIM, when, RESET);
    printf("  %-9s %s\n", "Memory", mem);
    printf("  %-9s %s %s%s%s\n", "Address", l->addr, DIM,
           wp_is_loopback(l->addr) ? "(only this computer)" : "(reachable from your network)", RESET);
    printf("\n  %sStop it: whoport %d --kill%s\n\n", DIM, l->port, RESET);
}

static int alive(int pid) {
    return kill(pid, 0) == 0 || errno == EPERM;
}

static int wait_for_exit(int pid, int ms) {
    struct timespec step = {0, 100 * 1000 * 1000};
    for (int waited = 0; waited < ms; waited += 100) {
        if (!alive(pid)) return 1;
        nanosleep(&step, NULL);
    }
    return !alive(pid);
}

static int stop(const listener_t *l, int force) {
    if (l->pid < 0) {
        fprintf(stderr, "  %sPort %d belongs to another user. Try: sudo whoport %d --kill%s\n", RED, l->port,
                l->port, RESET);
        return 2;
    }
    char project[WP_PATH_MAX], cmd[WP_CMD_MAX];
    project_of(l, project, sizeof project);
    wp_short_command(l->command, home, cmd, sizeof cmd);
    if (kill(l->pid, SIGTERM) != 0) {
        fprintf(stderr, "  %sCould not stop pid %d: %s%s\n", RED, l->pid, strerror(errno), RESET);
        return 2;
    }
    if (!wait_for_exit(l->pid, 3000)) {
        if (!force) {
            fprintf(stderr, "  %s\"%s\" did not stop within 3 seconds. Use --kill --force to make it.%s\n", YELLOW,
                    cmd, RESET);
            return 2;
        }
        kill(l->pid, SIGKILL);
        if (!wait_for_exit(l->pid, 2000)) {
            fprintf(stderr, "  %sCould not stop pid %d.%s\n", RED, l->pid, RESET);
            return 2;
        }
    }
    printf("  %s✓%s Stopped %s\"%s\"%s in %s. Port %d is free now.\n", GREEN, RESET, BOLD, cmd, RESET, project,
           l->port);
    return 0;
}

int main(int argc, char **argv) {
    int ports[MAX_QUERY], nports = 0;
    int do_kill = 0, force = 0, json = 0;
    color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version")) {
            printf("whoport %s\n", WHOPORT_VERSION);
            return 0;
        } else if (!strcmp(a, "-k") || !strcmp(a, "--kill")) {
            do_kill = 1;
        } else if (!strcmp(a, "-f") || !strcmp(a, "--force")) {
            force = 1;
        } else if (!strcmp(a, "-j") || !strcmp(a, "--json")) {
            json = 1;
        } else if (!strcmp(a, "--no-color")) {
            color = 0;
        } else if (!strcmp(a, "--color")) {
            color = 1;
        } else if (a[0] == ':' && wp_parse_port(a + 1) > 0 && nports < MAX_QUERY) {
            ports[nports++] = wp_parse_port(a + 1); /* ":3000" works too */
        } else if (wp_parse_port(a) > 0 && nports < MAX_QUERY) {
            ports[nports++] = wp_parse_port(a);
        } else {
            fprintf(stderr, "whoport: unknown argument '%s' (see whoport --help)\n", a);
            return 2;
        }
    }
    if (do_kill && !nports) {
        fprintf(stderr, "whoport: --kill needs a port, e.g. whoport 3000 --kill\n");
        return 2;
    }
    if (json) color = 0;

    home = getenv("HOME");
    if (!home || !*home) {
        struct passwd *pw = getpwuid(getuid());
        home = pw ? pw->pw_dir : NULL;
    }

    listener_list list = {0};
    if (wp_collect(&list) != 0) {
        fprintf(stderr, "whoport: could not read the list of open ports\n");
        return 2;
    }
    time_t now = time(NULL);

    if (!nports) {
        if (json) print_json(&list, now);
        else print_table(&list, now);
        wp_list_free(&list);
        return 0;
    }

    /* Keep only the queried ports, in the order they were asked for. */
    listener_list picked = {0};
    int status = 0, any_used = 0;
    for (int q = 0; q < nports; q++) {
        int found = 0;
        for (size_t i = 0; i < list.len; i++) {
            if (list.items[i].port != ports[q]) continue;
            listener_t *dst = wp_list_push(&picked);
            if (dst) *dst = list.items[i];
            found = 1;
        }
        if (!found && !json) printf("\n  %sPort %d is free.%s\n", GREEN, ports[q], RESET);
        any_used |= found;
    }
    if (!any_used) status = 1;

    if (json) {
        print_json(&picked, now);
    } else if (do_kill) {
        if (picked.len) printf("\n");
        for (size_t i = 0; i < picked.len; i++) {
            int r = stop(&picked.items[i], force);
            if (r) status = r;
        }
        if (picked.len) printf("\n");
        else printf("\n");
    } else {
        for (size_t i = 0; i < picked.len; i++) print_detail(&picked.items[i], now);
        if (!any_used) printf("\n");
    }
    wp_list_free(&picked);
    wp_list_free(&list);
    return status;
}
