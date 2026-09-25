/* whoport: see which project is running on which port, and stop it. */
#define _DEFAULT_SOURCE
#include "whoport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
            "  whoport --free [port]    print the first free port from [port] (default 3000)\n"
            "\n"
            "Options\n"
            "  -k, --kill      stop the process (SIGTERM, then waits up to 3 seconds)\n"
            "  -f, --force     with --kill: use SIGKILL if it does not stop in time\n"
            "  -a, --all       also show operating system services and other users' ports\n"
            "  -j, --json      machine-readable output\n"
            "      --color     force colours, e.g. when piping into less -R\n"
            "      --no-color  disable colours (also: NO_COLOR=1)\n"
            "  -h, --help      show this help\n"
            "  -v, --version   show the version\n"
            "\n"
            "Docker containers are shown by name; --kill stops the container.\n"
            "Exit status: 0 if a queried port is in use, 1 if it is free, 2 on errors.\n"
            "Processes of other users are only visible with sudo (Linux, macOS)\n"
            "or from an administrator terminal (Windows).\n",
            WHOPORT_VERSION);
}

/* Project folder of a listener, shortened for display. */
static void project_of(const listener_t *l, char *out, size_t size) {
    if (l->container[0]) {
        if (l->compose_dir[0]) wp_shorten_home(l->compose_dir, home, out, size);
        else snprintf(out, size, "(docker)");
        return;
    }
    if (l->pid < 0 || (l->restricted && !l->cwd[0])) {
        snprintf(out, size, "?");
        return;
    }
    if (wp_is_system_dir(l->cwd)) {
        snprintf(out, size, "(system)");
        return;
    }
    char root[WP_PATH_MAX];
    wp_find_project_root(l->cwd, home, root, sizeof root);
    wp_shorten_home(root, home, out, size);
}

/* What to show in the COMMAND column. */
static void command_of(const listener_t *l, char *out, size_t size) {
    if (l->container[0]) {
        snprintf(out, size, "container %s (%s)", l->container, l->image);
        return;
    }
    wp_short_command(l->command, home, out, size);
    if (l->restricted) {
        size_t n = strlen(out);
        if (n + 2 < size) memcpy(out + n, " *", 3);
    }
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
        if (wp_is_path(in)) snprintf(out, size, "...%s", in + len - (width - 3));
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
        if (l->pid < 0 && !l->container[0]) {
            printf(", \"pid\": null}");
            continue;
        }
        if (l->pid >= 0) printf(", \"pid\": %d, \"name\": ", l->pid);
        else printf(", \"pid\": null, \"name\": ");
        json_string(l->name);
        printf(", \"command\": ");
        json_string(l->command);
        printf(", \"cwd\": ");
        json_string(l->cwd);
        printf(", \"project\": ");
        json_string(project);
        printf(", \"uptime_seconds\": %lld, \"memory_bytes\": %lld, \"restricted\": %s, \"container\": ",
               uptime_of(l, now), l->rss, l->restricted ? "true" : "false");
        if (l->container[0]) {
            printf("{\"name\": ");
            json_string(l->container);
            printf(", \"image\": ");
            json_string(l->image);
            printf(", \"compose_dir\": ");
            json_string(l->compose_dir);
            printf("}}");
        } else {
            printf("null}");
        }
    }
    printf("%s]\n", list->len ? "\n" : "");
}

static int shown(const listener_t *l, int all) {
    return all || l->container[0] || (l->pid >= 0 && !wp_is_os_noise(l));
}

static void print_table(const listener_list *list, time_t now, int all) {
    int visible = 0, noise = 0, others = 0, restricted = 0;
    for (size_t i = 0; i < list->len; i++) {
        const listener_t *l = &list->items[i];
        if (shown(l, all)) {
            visible++;
            restricted += l->restricted;
        } else if (l->pid < 0) {
            others++;
        } else {
            noise++;
        }
    }
    if (!visible) {
        printf("\n  No listening ports found%s.\n", noise + others ? " besides system services (see --all)" : "");
        printf("\n");
        return;
    }
    int w_proj = 7, w_cmd = 7;
    char proj[WP_PATH_MAX], cmd[WP_CMD_MAX], cell[WP_PATH_MAX];
    for (size_t i = 0; i < list->len; i++) {
        if (!shown(&list->items[i], all)) continue;
        project_of(&list->items[i], proj, sizeof proj);
        command_of(&list->items[i], cmd, sizeof cmd);
        if ((int)strlen(proj) > w_proj) w_proj = (int)strlen(proj);
        if ((int)strlen(cmd) > w_cmd) w_cmd = (int)strlen(cmd);
    }
    if (w_proj > 30) w_proj = 30;
    if (w_cmd > 36) w_cmd = 36;

    printf("\n  %s%-6s %-*s  %-*s  %7s  %9s  %8s%s\n", DIM, "PORT", w_proj, "PROJECT", w_cmd, "COMMAND", "PID",
           "RUNNING", "MEMORY", RESET);
    int stale = 0, stale_port = 0;
    for (size_t i = 0; i < list->len; i++) {
        const listener_t *l = &list->items[i];
        if (!shown(l, all)) continue;
        if (l->pid < 0 && !l->container[0]) {
            printf("  %s%-6d%s %s%-*s  %s%s\n", BOLD, l->port, RESET, DIM, w_proj, "?", "(another user, try sudo)",
                   RESET);
            continue;
        }
        char up[32], mem[32], pid[16];
        long long secs = uptime_of(l, now);
        wp_format_duration(secs, up, sizeof up);
        wp_format_bytes(l->rss, mem, sizeof mem);
        if (l->pid >= 0) snprintf(pid, sizeof pid, "%d", l->pid);
        else snprintf(pid, sizeof pid, "-");
        project_of(l, proj, sizeof proj);
        command_of(l, cmd, sizeof cmd);

        int is_stale = secs >= LONG_RUNNING && home && strncmp(l->cwd, home, strlen(home)) == 0;
        if (is_stale) {
            stale++;
            stale_port = l->port;
        }
        printf("  %s%-6d%s ", BOLD, l->port, RESET);
        fit(proj, w_proj, cell, sizeof cell);
        printf("%s%-*s%s  ", l->container[0] ? "" : CYAN, w_proj, cell, RESET);
        fit(cmd, w_cmd, cell, sizeof cell);
        printf("%-*s  %s%7s%s  %s%9s%s  %8s", w_cmd, cell, DIM, pid, RESET, is_stale ? YELLOW : "", up, RESET, mem);
        if (is_stale) printf("  %s<- forgotten?%s", YELLOW, RESET);
        printf("\n");
    }
    printf("\n");
    if (stale == 1) printf("  %sRunning for more than a day. Stop it with: whoport %d --kill%s\n\n", DIM, stale_port, RESET);
    else if (stale > 1) printf("  %s%d servers have been running for more than a day.%s\n\n", DIM, stale, RESET);
    if (restricted)
        printf("  %s* Folder, uptime and memory need %s.%s\n", DIM,
#ifdef _WIN32
               "an administrator terminal",
#else
               "sudo",
#endif
               RESET);
    if (noise || others) {
        printf("  %sHidden:", DIM);
        if (noise) printf(" %d system service%s", noise, noise == 1 ? "" : "s");
        if (noise && others) printf(",");
        if (others) printf(" %d port%s of other users", others, others == 1 ? "" : "s");
        printf(". Show them with: whoport --all%s\n", RESET);
    }
    if (restricted || noise || others) printf("\n");
}

static void print_detail(const listener_t *l, time_t now) {
    char project[WP_PATH_MAX], up[32], mem[32], when[32];
    if (l->pid < 0 && !l->container[0]) {
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
        wp_localtime(l->started, &tm);
        strftime(when, sizeof when, now - t < 86400 ? "since %H:%M" : "since %b %d, %H:%M", &tm);
    }
    if (l->container[0]) {
        printf("\n  %sPort %d%s is published by Docker container %s%s%s\n\n", BOLD, l->port, RESET, BOLD, l->container,
               RESET);
        printf("  %-9s %s\n", "Image", l->image);
        if (l->compose_dir[0]) printf("  %-9s %s%s%s\n", "Project", CYAN, project, RESET);
        printf("  %-9s %s\n", "Address", l->addr);
        printf("\n  %sStop it: whoport %d --kill   (stops the container)%s\n\n", DIM, l->port, RESET);
        return;
    }
    printf("\n  %sPort %d%s is used by %s%s%s %s(pid %d)%s\n\n", BOLD, l->port, RESET, BOLD, l->name, RESET, DIM,
           l->pid, RESET);
    printf("  %-9s %s%s%s\n", "Project", CYAN, project, RESET);
    printf("  %-9s %s\n", "Command", l->command);
    if (l->restricted) {
        printf("  %-9s %s(run as administrator to see uptime, memory and folder)%s\n", "Details", DIM, RESET);
    } else {
        printf("  %-9s %s %s%s%s\n", "Running", up, DIM, when, RESET);
        printf("  %-9s %s\n", "Memory", mem);
    }
    printf("  %-9s %s %s%s%s\n", "Address", l->addr, DIM,
           wp_is_loopback(l->addr) ? "(only this computer)" : "(reachable from your network)", RESET);
    printf("\n  %sStop it: whoport %d --kill%s\n\n", DIM, l->port, RESET);
}

static int wait_for_exit(int pid, int ms) {
    for (int waited = 0; waited < ms; waited += 100) {
        if (!wp_is_alive(pid)) return 1;
        wp_sleep_ms(100);
    }
    return !wp_is_alive(pid);
}

static int stop(const listener_t *l, int force) {
    if (l->container[0]) {
        char err[256] = "";
        if (wp_docker_stop(l->container, err, sizeof err) != 0) {
            fprintf(stderr, "  %sCould not stop container %s: %s%s\n", RED, l->container, err, RESET);
            return 2;
        }
        printf("  %s✓%s Stopped container %s%s%s. Port %d is free now.\n", GREEN, RESET, BOLD, l->container, RESET,
               l->port);
        return 0;
    }
    if (l->pid < 0) {
        fprintf(stderr, "  %sPort %d belongs to another user. Try: sudo whoport %d --kill%s\n", RED, l->port,
                l->port, RESET);
        return 2;
    }
    char project[WP_PATH_MAX], cmd[WP_CMD_MAX];
    project_of(l, project, sizeof project);
    wp_short_command(l->command, home, cmd, sizeof cmd);
    char err[256] = "";
    if (wp_terminate(l->pid, 0, err, sizeof err) != 0) {
        fprintf(stderr, "  %sCould not stop pid %d: %s%s\n", RED, l->pid, err, RESET);
        return 2;
    }
    if (!wait_for_exit(l->pid, 3000)) {
        if (!force) {
            fprintf(stderr, "  %s\"%s\" did not stop within 3 seconds. Use --kill --force to make it.%s\n", YELLOW,
                    cmd, RESET);
            return 2;
        }
        wp_terminate(l->pid, 1, err, sizeof err);
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
    int do_kill = 0, force = 0, json = 0, all = 0, free_mode = 0;
    wp_platform_init();
    color = wp_stdout_is_tty() && !getenv("NO_COLOR");

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
        } else if (!strcmp(a, "--free")) {
            free_mode = 1;
        } else if (!strcmp(a, "-a") || !strcmp(a, "--all")) {
            all = 1;
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
    if (free_mode && (do_kill || nports > 1)) {
        fprintf(stderr, "whoport: --free takes at most one start port, e.g. whoport --free 3000\n");
        return 2;
    }

    home = wp_home();

    listener_list list = {0};
    if (wp_collect(&list) != 0) {
        fprintf(stderr, "whoport: could not read the list of open ports\n");
        return 2;
    }
    time_t now = time(NULL);

    /* --free: first port from the start that nothing listens on and that can
     * actually be bound. Prints only the number, so scripts can use it. */
    if (free_mode) {
        int start = nports ? ports[0] : 3000;
        for (int p = start; p <= 65535; p++) {
            int taken = 0;
            for (size_t i = 0; i < list.len && !taken; i++) taken = list.items[i].port == p;
            if (!taken && wp_port_bindable(p)) {
                if (json) printf("{\"port\": %d}\n", p);
                else printf("%d\n", p);
                wp_list_free(&list);
                return 0;
            }
        }
        fprintf(stderr, "whoport: no free port from %d\n", start);
        wp_list_free(&list);
        return 1;
    }

    /* Ports published by Docker belong to a container, not to the Docker
     * daemon or proxy process that holds the socket. */
    wp_container *containers = NULL;
    size_t n_containers = 0;
    if (wp_docker_containers(&containers, &n_containers) == 0) {
        for (size_t k = 0; k < n_containers; k++) {
            int matched = 0;
            for (size_t i = 0; i < list.len; i++) {
                if (containers[k].port != list.items[i].port) continue;
                listener_t *l = &list.items[i];
                wp_copy(l->container, sizeof l->container, containers[k].name);
                wp_copy(l->image, sizeof l->image, containers[k].image);
                wp_copy(l->compose_dir, sizeof l->compose_dir, containers[k].workdir);
                matched = 1;
            }
            /* Without a userland proxy Docker forwards ports in the kernel, so
             * no process holds the socket. Show the container anyway. */
            if (!matched) {
                listener_t *l = wp_list_push(&list);
                if (!l) break;
                l->port = containers[k].port;
                wp_copy(l->addr, sizeof l->addr, "0.0.0.0");
                wp_copy(l->container, sizeof l->container, containers[k].name);
                wp_copy(l->image, sizeof l->image, containers[k].image);
                wp_copy(l->compose_dir, sizeof l->compose_dir, containers[k].workdir);
            }
        }
        free(containers);
        wp_list_sort_dedupe(&list);
    }

    if (!nports) {
        if (json) print_json(&list, now);
        else print_table(&list, now, all);
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
