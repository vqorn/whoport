/* whoport: see which project is running on which port, and stop it. */
#define _DEFAULT_SOURCE
#include "whoport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <strings.h>
#include <time.h>

#ifndef WHOPORT_VERSION
#define WHOPORT_VERSION "dev"
#endif

#define MAX_QUERY 64
#define LONG_RUNNING (24 * 3600)

static int color = 1;
static int in_live = 0; /* --live: hints name keys instead of flags */
static const char *home = NULL;

#define C(code) (color ? "\033[" code "m" : "")
#define RESET C("0")
#define BOLD C("1")
#define DIM C("2")
#define RED C("31")
#define GREEN C("32")
#define YELLOW C("33")
#define CYAN C("36")
#define MAGENTA C("35")

static void usage(FILE *f) {
    fprintf(f,
            "whoport %s: see which project is running on which port.\n"
            "\n"
            "Usage\n"
            "  whoport                  list every listening port\n"
            "  whoport <port>...        show who is using these ports\n"
            "  whoport <port> --kill    stop the process on that port (also: whoport stop <port>)\n"
            "  whoport open <port>      open http://localhost:<port> in the browser\n"
            "  whoport stop [project]   stop every server of a project: a folder name, a path,\n"
            "                           or nothing for the project you are in\n"
            "  whoport --free [port]    print the first free port from [port] (default 3000)\n"
            "  whoport --live           live view: arrow keys select, o open, k stop,\n"
            "                           p stop the whole project, q quit\n"
            "  whoport --watch [port]   print a line whenever a port opens or closes\n"
            "  whoport --wait <port>    wait until something listens on the port\n"
            "\n"
            "Options\n"
            "  -k, --kill      stop the process (SIGTERM, then waits up to 3 seconds)\n"
            "  -f, --force     with --kill or stop: use SIGKILL if it does not stop in time\n"
            "  -y, --yes       with stop <project>: do not ask before stopping\n"
            "  -o, --open      same as whoport open <port>\n"
            "  -a, --all       also show operating system services and other users' ports\n"
            "  -j, --json      machine-readable output\n"
            "      --timeout N with --wait: give up after N seconds (default 60, 0 = never)\n"
            "      --color     force colours, e.g. when piping into less -R\n"
            "      --no-color  disable colours (also: NO_COLOR=1)\n"
            "  -h, --help      show this help (also: whoport help)\n"
            "  -v, --version   show the version\n"
            "\n"
            "Examples\n"
            "  whoport 3000                      who is on port 3000?\n"
            "  whoport 3000 5173                 several ports at once\n"
            "  whoport 3000 --kill               free port 3000\n"
            "  whoport open 5173                 open your dev server in the browser\n"
            "  whoport stop fontia               stop everything of ~/.../fontia, containers too\n"
            "  PORT=$(whoport --free 3000) npm run dev\n"
            "  docker compose up -d && whoport --wait 5432 && npm run dev\n"
            "\n"
            "Docker containers are shown by name; --kill stops the container.\n"
            "Exit status: 0 if a queried port is in use, 1 if it is free, 2 on errors.\n"
            "Processes of other users are only visible with sudo (Linux, macOS)\n"
            "or from an administrator terminal (Windows).\n"
            "Something wrong? Run with WHOPORT_DEBUG=1 and open an issue:\n"
            "https://github.com/vqorn/whoport/issues\n",
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

/* Your own code is highlighted; system services and installed apps are dimmed. */
static int is_project(const char *proj) {
    static const char *const apps[] = {"/AppData/", "Program Files", "/Library/", "/Applications/", "/opt/", "/snap/", NULL};
    if (proj[0] == '(' || proj[0] == '?' || !proj[0]) return 0;
    for (int i = 0; apps[i]; i++)
        if (strstr(proj, apps[i])) return 0;
    return 1;
}

/* What to show in the COMMAND column. */
static void command_of(const listener_t *l, char *out, size_t size) {
    if (l->container[0]) {
        snprintf(out, size, "docker %s", l->container); /* the image is in the detail view */
        return;
    }
    wp_short_command(l->command, home, out, size);
    if (l->restricted) {
        size_t n = strlen(out);
        if (n + 2 < size) memcpy(out + n, " *", 3);
    }
}

/* APP column: "Next.js", "PostgreSQL"... */
static void app_of(const listener_t *l, char *out, size_t size) {
    if (l->pid < 0 && !l->container[0]) out[0] = '\0';
    else wp_detect_app(l, out, size);
}

#define APP_MAX 14

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
        char app[32];
        app_of(l, app, sizeof app);
        printf(", \"app\": ");
        if (app[0]) json_string(app);
        else printf("null");
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
    int w_proj = 7, w_cmd = 7, w_app = 0;
    char proj[WP_PATH_MAX], cmd[WP_CMD_MAX], cell[WP_PATH_MAX], app[32];
    for (size_t i = 0; i < list->len; i++) {
        if (!shown(&list->items[i], all)) continue;
        project_of(&list->items[i], proj, sizeof proj);
        command_of(&list->items[i], cmd, sizeof cmd);
        app_of(&list->items[i], app, sizeof app);
        if ((int)strlen(proj) > w_proj) w_proj = (int)strlen(proj);
        if ((int)strlen(cmd) > w_cmd) w_cmd = (int)strlen(cmd);
        if ((int)strlen(app) > w_app) w_app = (int)strlen(app);
    }
    if (w_proj > 30) w_proj = 30;
    if (w_cmd > 36) w_cmd = 36;
    if (w_app > APP_MAX) w_app = APP_MAX;
    if (w_app && w_app < 3) w_app = 3;
    /* Never wrap: shrink the two text columns to fit the terminal. A row is
     * 41 characters plus the text columns. */
    int base = 41 + (w_app ? w_app + 2 : 0);
    int term = wp_term_width();
    if (term > 0) {
        int over = base + w_proj + w_cmd - (term - 1);
        while (over > 0 && (w_proj > 16 || w_cmd > 20)) {
            if (w_proj > 16 && (w_proj >= w_cmd || w_cmd <= 20)) w_proj--;
            else w_cmd--;
            over--;
        }
    }
    int room_for_hint = term <= 0 || base + w_proj + w_cmd + 15 <= term - 1;

    printf("\n  %s%-6s %-*s  ", DIM, "PORT", w_proj, "PROJECT");
    if (w_app) printf("%-*s  ", w_app, "APP");
    printf("%-*s  %7s  %9s  %8s%s\n", w_cmd, "COMMAND", "PID", "RUNNING", "MEMORY", RESET);
    int stale = 0, stale_port = 0;
    for (size_t i = 0; i < list->len; i++) {
        const listener_t *l = &list->items[i];
        if (!shown(l, all)) continue;
        if (l->pid < 0 && !l->container[0]) {
            printf("  %s%-6d%s %s%-*s  %*s%s%s\n", BOLD, l->port, RESET, DIM, w_proj, "?", w_app ? w_app + 2 : 0, "",
                   "(another user, try sudo)", RESET);
            continue;
        }
        char up[32], mem[32], pid[16];
        long long secs = uptime_of(l, now);
        wp_format_duration(secs, up, sizeof up);
        wp_format_bytes(l->rss, mem, sizeof mem);
        /* For a container, pid and memory would be Docker's own, not the container's. */
        if (l->pid >= 0 && !l->container[0]) snprintf(pid, sizeof pid, "%d", l->pid);
        else snprintf(pid, sizeof pid, "-");
        if (l->container[0]) snprintf(mem, sizeof mem, "-");
        project_of(l, proj, sizeof proj);
        command_of(l, cmd, sizeof cmd);

        /* Only your own projects can be "forgotten", not system services. */
        int in_home = l->container[0] ? proj[0] == '~' : home && strncmp(l->cwd, home, strlen(home)) == 0;
        int is_stale = secs >= LONG_RUNNING && in_home;
        if (is_stale) {
            stale++;
            stale_port = l->port;
        }
        printf("  %s%-6d%s ", BOLD, l->port, RESET);
        fit(proj, w_proj, cell, sizeof cell);
        printf("%s%-*s%s  ", is_project(proj) ? CYAN : DIM, w_proj, cell, RESET);
        if (w_app) {
            app_of(l, app, sizeof app);
            fit(app, w_app, cell, sizeof cell);
            printf("%s%-*s%s  ", MAGENTA, w_app, cell, RESET);
        }
        fit(cmd, w_cmd, cell, sizeof cell);
        printf("%-*s  %s%7s%s  %s%9s%s  %8s", w_cmd, cell, DIM, pid, RESET, is_stale ? YELLOW : "", up, RESET, mem);
        if (is_stale && room_for_hint) printf("  %s<- forgotten?%s", YELLOW, RESET);
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
    char app[32];
    app_of(l, app, sizeof app);
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
        if (app[0]) printf("  %-9s %s%s%s\n", "App", MAGENTA, app, RESET);
        if (l->compose_dir[0]) printf("  %-9s %s%s%s\n", "Project", CYAN, project, RESET);
        if (l->started > 0) printf("  %-9s %s %s\n", "Running", up, when);
        printf("  %-9s %s\n", "Address", l->addr);
        printf("\n  %sStop it: whoport %d --kill   (stops the container)%s\n\n", DIM, l->port, RESET);
        return;
    }
    printf("\n  %sPort %d%s is used by %s%s%s %s(pid %d)%s\n\n", BOLD, l->port, RESET, BOLD, l->name, RESET, DIM,
           l->pid, RESET);
    printf("  %-9s %s%s%s\n", "Project", CYAN, project, RESET);
    if (app[0]) printf("  %-9s %s%s%s\n", "App", MAGENTA, app, RESET);
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

/* Stops whatever holds the port. Writes a one-line result to msg and
 * returns 0 on success, 2 on failure. Used by --kill and by --live. */
static int stop_msg(const listener_t *l, int force, char *msg, size_t size) {
    char err[256] = "";
    if (l->container[0]) {
        if (wp_docker_stop(l->container, err, sizeof err) != 0) {
            snprintf(msg, size, "%sCould not stop container %s: %s%s", RED, l->container, err, RESET);
            return 2;
        }
        snprintf(msg, size, "%s✓%s Stopped container %s%s%s. Port %d is free now.", GREEN, RESET, BOLD, l->container,
                 RESET, l->port);
        return 0;
    }
    if (l->pid < 0) {
        snprintf(msg, size, "%sPort %d belongs to another user. Try: sudo whoport %d --kill%s", RED, l->port, l->port,
                 RESET);
        return 2;
    }
    char project[WP_PATH_MAX], cmd[WP_CMD_MAX];
    project_of(l, project, sizeof project);
    wp_short_command(l->command, home, cmd, sizeof cmd);
    if (wp_terminate(l->pid, 0, err, sizeof err) != 0) {
        snprintf(msg, size, "%sCould not stop pid %d: %s%s", RED, l->pid, err, RESET);
        return 2;
    }
    if (!wait_for_exit(l->pid, 3000)) {
        if (!force) {
            snprintf(msg, size, "%s\"%s\" did not stop within 3 seconds. %s%s", YELLOW, cmd,
                     in_live ? "Press K to force it." : "Use --kill --force to make it.", RESET);
            return 2;
        }
        wp_terminate(l->pid, 1, err, sizeof err);
        if (!wait_for_exit(l->pid, 2000)) {
            snprintf(msg, size, "%sCould not stop pid %d.%s", RED, l->pid, RESET);
            return 2;
        }
    }
    snprintf(msg, size, "%s✓%s Stopped %s\"%s\"%s in %s. Port %d is free now.", GREEN, RESET, BOLD, cmd, RESET,
             project, l->port);
    return 0;
}

static int stop(const listener_t *l, int force) {
    char msg[WP_CMD_MAX + WP_PATH_MAX + 256];
    int r = stop_msg(l, force, msg, sizeof msg);
    fprintf(r ? stderr : stdout, "  %s\n", msg);
    return r;
}

/* Ports published by Docker belong to a container, not to the Docker
 * daemon or proxy process that holds the socket. */
static void attach_containers(listener_list *list) {
    wp_container *containers = NULL;
    size_t n_containers = 0;
    if (wp_docker_containers(&containers, &n_containers) == 0) {
        for (size_t k = 0; k < n_containers; k++) {
            int matched = 0;
            for (size_t i = 0; i < list->len; i++) {
                if (containers[k].port != list->items[i].port) continue;
                listener_t *l = &list->items[i];
                wp_copy(l->container, sizeof l->container, containers[k].name);
                wp_copy(l->image, sizeof l->image, containers[k].image);
                wp_copy(l->compose_dir, sizeof l->compose_dir, containers[k].workdir);
                l->started = containers[k].started > 0   ? containers[k].started
                             : containers[k].created > 0 ? containers[k].created
                                                         : -1;
                l->rss = -1;
                matched = 1;
            }
            /* Without a userland proxy Docker forwards ports in the kernel, so
             * no process holds the socket. Show the container anyway. */
            if (!matched) {
                listener_t *l = wp_list_push(list);
                if (!l) break;
                l->port = containers[k].port;
                wp_copy(l->addr, sizeof l->addr, "0.0.0.0");
                wp_copy(l->container, sizeof l->container, containers[k].name);
                wp_copy(l->image, sizeof l->image, containers[k].image);
                wp_copy(l->compose_dir, sizeof l->compose_dir, containers[k].workdir);
                l->started = containers[k].started > 0   ? containers[k].started
                             : containers[k].created > 0 ? containers[k].created
                                                         : -1;
                l->rss = -1;
            }
        }
        free(containers);
        wp_list_sort_dedupe(list);
    }
}

/* Everything that listens right now, with containers attached. */
static int snapshot(listener_list *list) {
    memset(list, 0, sizeof *list);
    if (wp_collect(list) != 0) return -1;
    attach_containers(list);
    return 0;
}

/* ---------- open, stop <project> ---------- */

/* Opens the port in the browser. 0 when opened, 2 when not. */
static int open_msg(const listener_t *l, char *msg, size_t size) {
    char app[32], url[64];
    int http = wp_detect_app(l, app, sizeof app);
    if (http == 0) {
        snprintf(msg, size, "%s%s on port %d does not speak HTTP, there is nothing to open in a browser.%s", YELLOW, app,
                 l->port, RESET);
        return 2;
    }
    snprintf(url, sizeof url, "%s://localhost:%d", l->port == 443 || l->port == 8443 ? "https" : "http", l->port);
    if (wp_open_url(url) != 0) {
        snprintf(msg, size, "%sCould not start a browser. Open %s yourself.%s", RED, url, RESET);
        return 2;
    }
    snprintf(msg, size, "%s✓%s Opened %s%s%s", GREEN, RESET, BOLD, url, RESET);
    return 0;
}

static void slashes(char *p) {
    for (; *p; p++)
        if (*p == '\\') *p = '/';
}

static void strip_slash(char *p) {
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/' && !(n == 3 && p[1] == ':')) p[--n] = '\0';
}

/* Full project folder of a listener, "" if unknown. */
static void root_of(const listener_t *l, char *out, size_t size) {
    out[0] = '\0';
    if (l->container[0]) wp_copy(out, size, l->compose_dir);
    else if (l->pid >= 0 && l->cwd[0]) wp_find_project_root(l->cwd, home, out, size);
    slashes(out);
    strip_slash(out);
}

static int same_path(const char *a, const char *b) {
    int windows = isalpha((unsigned char)a[0]) && a[1] == ':';
    return windows ? strcasecmp(a, b) == 0 : strcmp(a, b) == 0;
}

static const char *base_name(const char *p) {
    const char *slash = strrchr(p, '/');
    return slash ? slash + 1 : p;
}

/* Turns "fontia", "~/code/shop", "." or nothing into a folder to match.
 * *by_name is set when only the last part of the folder should match. */
static int resolve_project(const char *arg, char *target, size_t size, int *by_name) {
    char cwd[WP_PATH_MAX];
    *by_name = 0;
    if (!arg || !strcmp(arg, ".") || !strcmp(arg, "./")) {
        if (wp_getcwd(cwd, sizeof cwd) != 0) return -1;
        slashes(cwd);
        wp_find_project_root(cwd, home, target, size);
    } else if (strchr(arg, '/') || strchr(arg, '\\') || arg[0] == '~') {
        if (arg[0] == '~' && home) snprintf(target, size, "%s%s", home, arg + 1);
        else if (arg[0] == '/' || (isalpha((unsigned char)arg[0]) && arg[1] == ':')) wp_copy(target, size, arg);
        else {
            if (wp_getcwd(cwd, sizeof cwd) != 0) return -1;
            int n = snprintf(target, size, "%s/%s", cwd, arg[0] == '.' && (arg[1] == '/' || arg[1] == '\\') ? arg + 2 : arg);
            if (n < 0 || (size_t)n >= size) return -1;
        }
    } else {
        wp_copy(target, size, arg);
        *by_name = 1;
        return 0;
    }
    slashes(target);
    strip_slash(target);
    return 0;
}

/* Indices of the servers that belong to the project, one per process or container. */
static int project_targets(const listener_list *list, const char *target, int by_name, int *idx, int max,
                           char *root_out, size_t root_size, int *roots) {
    int n = 0;
    *roots = 0;
    root_out[0] = '\0';
    for (size_t i = 0; i < list->len && n < max; i++) {
        const listener_t *l = &list->items[i];
        if (l->pid < 0 && !l->container[0]) continue;
        char root[WP_PATH_MAX];
        root_of(l, root, sizeof root);
        if (!root[0]) continue;
        if (by_name ? strcasecmp(base_name(root), target) != 0 : !same_path(root, target)) continue;
        int dup = 0;
        for (int k = 0; k < n && !dup; k++) {
            const listener_t *o = &list->items[idx[k]];
            dup = l->container[0] ? !strcmp(o->container, l->container) : !o->container[0] && o->pid == l->pid;
        }
        if (dup) continue;
        if (!root_out[0]) {
            wp_copy(root_out, root_size, root);
            *roots = 1;
        } else if (!same_path(root_out, root)) {
            (*roots)++;
        }
        idx[n++] = (int)i;
    }
    return n;
}

static int is_home_or_root(const char *dir) {
    return !dir[0] || !strcmp(dir, "/") || (home && same_path(dir, home)) || (strlen(dir) <= 3 && dir[1] == ':');
}

/* whoport stop [project] */
static int stop_project(const listener_list *list, const char *arg, int force, int yes) {
    char target[WP_PATH_MAX], root[WP_PATH_MAX], shown_root[WP_PATH_MAX];
    int by_name, idx[256], roots;
    if (resolve_project(arg, target, sizeof target, &by_name) != 0) {
        fprintf(stderr, "whoport: could not read the current folder\n");
        return 2;
    }
    if (!by_name && is_home_or_root(target)) {
        fprintf(stderr, "whoport: %s is not a project folder. Run this inside a project, or name it: whoport stop myapp\n",
                target[0] ? target : "this");
        return 2;
    }
    int n = project_targets(list, target, by_name, idx, 256, root, sizeof root, &roots);
    if (!n) {
        wp_shorten_home(target, home, shown_root, sizeof shown_root);
        printf("\n  %sNothing is running in %s.%s\n\n", GREEN, shown_root, RESET);
        return 1;
    }
    if (roots > 1) {
        fprintf(stderr, "\n  %sSeveral projects are called \"%s\":%s\n", YELLOW, target, RESET);
        for (int k = 0; k < n; k++) {
            char r[WP_PATH_MAX];
            root_of(&list->items[idx[k]], r, sizeof r);
            wp_shorten_home(r, home, shown_root, sizeof shown_root);
            fprintf(stderr, "    %s\n", shown_root);
        }
        fprintf(stderr, "\n  Name the folder instead, e.g. whoport stop %s\n\n", shown_root);
        return 2;
    }
    wp_shorten_home(root, home, shown_root, sizeof shown_root);
    printf("\n  %s%s%s%s: %d server%s\n\n", BOLD, CYAN, shown_root, RESET, n, n == 1 ? "" : "s");
    for (int k = 0; k < n; k++) {
        const listener_t *l = &list->items[idx[k]];
        char cmd[WP_CMD_MAX], app[32];
        command_of(l, cmd, sizeof cmd);
        app_of(l, app, sizeof app);
        printf("    %s%-6d%s %s%-12s%s %s\n", BOLD, l->port, RESET, MAGENTA, app, RESET, cmd);
    }
    if (!yes) {
        if (!wp_stdin_is_tty()) {
            fflush(stdout);
            fprintf(stderr, "\n  whoport: add --yes to stop them without asking\n\n");
            return 2;
        }
        printf("\n  Stop %s? [y/N] ", n == 1 ? "it" : "all of them");
        fflush(stdout);
        char answer[16] = "";
        if (!fgets(answer, sizeof answer, stdin) || (answer[0] != 'y' && answer[0] != 'Y')) {
            printf("  Nothing stopped.\n\n");
            return 1;
        }
    }
    printf("\n");
    int status = 0;
    for (int k = 0; k < n; k++) {
        int r = stop(&list->items[idx[k]], force);
        if (r) status = r;
    }
    printf("\n");
    return status;
}

/* ---------- --wait, --watch ---------- */

static const listener_t *find_port(const listener_list *list, int port) {
    for (size_t i = 0; i < list->len; i++)
        if (list->items[i].port == port) return &list->items[i];
    return NULL;
}

/* "node server.js  ~/code/shop" for event lines. */
static void describe(const listener_t *l, char *out, size_t size) {
    char proj[WP_PATH_MAX], cmd[WP_CMD_MAX];
    project_of(l, proj, sizeof proj);
    command_of(l, cmd, sizeof cmd);
    snprintf(out, size, "%s  %s%s%s", cmd, is_project(proj) ? CYAN : DIM, proj, RESET);
}

static void clock_now(char *out, size_t size) {
    struct tm tm;
    wp_localtime((long long)time(NULL), &tm);
    strftime(out, size, "%H:%M:%S", &tm);
}

/* --wait: block until every given port is in use. 0 when they are, 1 on timeout. */
static int wait_mode(const int *ports, int nports, int timeout_s) {
    long long deadline = timeout_s > 0 ? wp_clock_ms() + timeout_s * 1000LL : 0;
    int announced = 0;
    for (;;) {
        listener_list list;
        if (snapshot(&list) != 0) {
            fprintf(stderr, "whoport: could not read the list of open ports\n");
            return 2;
        }
        int missing = -1;
        for (int q = 0; q < nports && missing < 0; q++)
            if (!find_port(&list, ports[q])) missing = ports[q];
        if (missing < 0) {
            for (int q = 0; q < nports; q++) {
                char what[WP_CMD_MAX + WP_PATH_MAX + 64];
                describe(find_port(&list, ports[q]), what, sizeof what);
                fprintf(stderr, "  %s✓%s Port %s%d%s is up: %s\n", GREEN, RESET, BOLD, ports[q], RESET, what);
            }
            wp_list_free(&list);
            return 0;
        }
        wp_list_free(&list);
        if (deadline && wp_clock_ms() >= deadline) {
            fprintf(stderr, "  %sPort %d is still free after %d second%s.%s\n", RED, missing, timeout_s, timeout_s == 1 ? "" : "s", RESET);
            return 1;
        }
        if (!announced) {
            fprintf(stderr, "  %sWaiting for port %d...%s\n", DIM, missing, RESET);
            announced = 1;
        }
        wp_sleep_ms(300);
    }
}

/* Identity of whatever holds a port, to notice when it changes. */
static int same_owner(const listener_t *a, const listener_t *b) {
    return a->pid == b->pid && !strcmp(a->container, b->container);
}

static void event(const char *what, const char *arrow, const char *col, int port, const listener_t *l, int bell) {
    char t[16], desc[WP_CMD_MAX + WP_PATH_MAX + 64] = "";
    clock_now(t, sizeof t);
    if (l) describe(l, desc, sizeof desc);
    printf("  %s%s%s  %s%-5d%s  %s%s %-6s%s  %s\n", DIM, t, RESET, BOLD, port, RESET, col, arrow, what, RESET, desc);
    if (bell) putchar('\a');
    fflush(stdout);
}

/* --watch: print a line whenever a port opens or closes. Runs until Ctrl+C. */
static int watch_mode(const int *ports, int nports, int all) {
    int bell = wp_stdout_is_tty();
    listener_list prev;
    if (snapshot(&prev) != 0) {
        fprintf(stderr, "whoport: could not read the list of open ports\n");
        return 2;
    }
    if (nports) {
        printf("\n  %sWatching port%s", DIM, nports > 1 ? "s" : "");
        for (int q = 0; q < nports; q++) printf("%s %d", q ? "," : "", ports[q]);
        printf(". Ctrl+C to stop.%s\n\n", RESET);
        for (int q = 0; q < nports; q++) {
            const listener_t *l = find_port(&prev, ports[q]);
            if (l) event("in use", "●", CYAN, ports[q], l, 0);
            else event("free", "○", DIM, ports[q], NULL, 0);
        }
    } else {
        int n = 0;
        for (size_t i = 0; i < prev.len; i++) n += shown(&prev.items[i], all) && find_port(&prev, prev.items[i].port) == &prev.items[i];
        printf("\n  %sWatching all ports (%d in use now). Ctrl+C to stop.%s\n\n", DIM, n, RESET);
    }
    fflush(stdout);
    for (;;) {
        wp_sleep_ms(1000);
        listener_list cur;
        if (snapshot(&cur) != 0) continue;
        /* Closed or taken over. */
        for (size_t i = 0; i < prev.len; i++) {
            const listener_t *old = &prev.items[i];
            if (find_port(&prev, old->port) != old) continue; /* first entry per port only */
            int wanted = 0;
            for (int q = 0; q < nports; q++) wanted |= ports[q] == old->port;
            if (nports ? !wanted : !shown(old, all)) continue;
            const listener_t *now_l = find_port(&cur, old->port);
            if (!now_l) event("closed", "▼", RED, old->port, old, bell);
            else if (!same_owner(old, now_l)) event("closed", "▼", RED, old->port, old, bell);
        }
        /* Opened (or opened by someone new). */
        for (size_t i = 0; i < cur.len; i++) {
            const listener_t *l = &cur.items[i];
            if (find_port(&cur, l->port) != l) continue;
            int wanted = 0;
            for (int q = 0; q < nports; q++) wanted |= ports[q] == l->port;
            if (nports ? !wanted : !shown(l, all)) continue;
            const listener_t *old = find_port(&prev, l->port);
            if (!old || !same_owner(old, l)) event("up", "▲", GREEN, l->port, l, bell);
        }
        wp_list_free(&prev);
        prev = cur;
    }
}

/* ---------- --live ---------- */

typedef struct {
    char *p;
    size_t len, cap;
} sbuf;

static void sb_printf(sbuf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        size_t room = b->cap - b->len;
        va_start(ap, fmt);
        int n = b->p ? vsnprintf(b->p + b->len, room, fmt, ap) : -1;
        va_end(ap);
        if (n >= 0 && (size_t)n < room) {
            b->len += (size_t)n;
            return;
        }
        size_t cap = b->cap ? b->cap * 2 : 16384;
        while (n >= 0 && cap - b->len <= (size_t)n) cap *= 2;
        char *grown = realloc(b->p, cap);
        if (!grown) return;
        b->p = grown;
        b->cap = cap;
    }
}

typedef struct {
    int port, pid;
    char container[128];
    long long first_seen; /* ms; 0 for everything that was there at the start */
} seen_t;

typedef struct {
    seen_t *items;
    size_t len, cap;
} seen_list;

static long long first_seen(seen_list *s, const listener_t *l, long long now_ms, int initial) {
    for (size_t i = 0; i < s->len; i++)
        if (s->items[i].port == l->port && s->items[i].pid == l->pid && !strcmp(s->items[i].container, l->container))
            return s->items[i].first_seen;
    if (s->len == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        seen_t *grown = realloc(s->items, cap * sizeof *grown);
        if (!grown) return 0;
        s->items = grown;
        s->cap = cap;
    }
    seen_t *e = &s->items[s->len++];
    e->port = l->port;
    e->pid = l->pid;
    wp_copy(e->container, sizeof e->container, l->container);
    e->first_seen = initial ? 0 : now_ms;
    return e->first_seen;
}

#define LIVE_NEW_MS 4000

/* Appends s, cut to max visible columns; escape sequences do not count. */
static void sb_fit(sbuf *b, const char *s, int max) {
    int cols = 0;
    const char *p = s;
    while (*p) {
        if (*p == '\033') {
            const char *q = p + 1;
            if (*q == '[') {
                q++;
                while (*q && !(*q >= '@' && *q <= '~')) q++;
                if (*q) q++;
            }
            sb_printf(b, "%.*s", (int)(q - p), p);
            p = q;
            continue;
        }
        if (((unsigned char)*p & 0xC0) != 0x80) {
            if (cols == max) break;
            cols++;
        }
        sb_printf(b, "%c", *p++);
    }
    sb_printf(b, "%s", RESET);
}

static void live_render(sbuf *b, const listener_list *list, const int *rows, int nrows, int sel, int *scroll,
                        int all, int hidden, seen_list *seen, const char *status) {
    int w = wp_term_width(), h = wp_term_height();
    if (w < 40) w = 80;
    if (h < 12) h = 24;
    time_t now = time(NULL);
    long long now_ms = wp_clock_ms();
    char t[16];
    clock_now(t, sizeof t);
    b->len = 0;
    sb_printf(b, "\033[H");
    sb_printf(b, "\n  %swhoport%s %slive  ·  %d port%s  ·  %s%s\033[K\n\n", BOLD, RESET, DIM, nrows, nrows == 1 ? "" : "s",
              t, RESET);

    int w_proj = 7, w_cmd = 7, w_app = 0;
    char proj[WP_PATH_MAX], cmd[WP_CMD_MAX], cell[WP_PATH_MAX], cell2[WP_CMD_MAX], app[32], cell3[32];
    for (int r = 0; r < nrows; r++) {
        project_of(&list->items[rows[r]], proj, sizeof proj);
        command_of(&list->items[rows[r]], cmd, sizeof cmd);
        app_of(&list->items[rows[r]], app, sizeof app);
        if ((int)strlen(proj) > w_proj) w_proj = (int)strlen(proj);
        if ((int)strlen(cmd) > w_cmd) w_cmd = (int)strlen(cmd);
        if ((int)strlen(app) > w_app) w_app = (int)strlen(app);
    }
    if (w_proj > 30) w_proj = 30;
    if (w_cmd > 36) w_cmd = 36;
    if (w_app > APP_MAX) w_app = APP_MAX;
    if (w_app && w_app < 3) w_app = 3;
    int w_appcol = w_app ? w_app + 2 : 0;
    int over = 43 + w_appcol + w_proj + w_cmd - (w - 1);
    while (over > 0 && (w_proj > 12 || w_cmd > 14)) {
        if (w_proj > 12 && (w_proj >= w_cmd || w_cmd <= 14)) w_proj--;
        else w_cmd--;
        over--;
    }
    sb_printf(b, "  %s  %-6s %-*s  %-*s%-*s  %7s  %9s  %8s%s\033[K\n", DIM, "PORT", w_proj, "PROJECT", w_appcol,
              w_app ? "APP" : "", w_cmd, "COMMAND", "PID", "RUNNING", "MEMORY", RESET);

    int body = h - 13; /* header 4, detail and keys 9 */
    if (body < 3) body = 3;
    if (sel < *scroll) *scroll = sel;
    if (sel >= *scroll + body) *scroll = sel - body + 1;
    if (*scroll > nrows - body) *scroll = nrows - body > 0 ? nrows - body : 0;
    for (int r = *scroll; r < nrows && r < *scroll + body; r++) {
        const listener_t *l = &list->items[rows[r]];
        char up[32], mem[32], pid[16];
        wp_format_duration(uptime_of(l, now), up, sizeof up);
        wp_format_bytes(l->rss, mem, sizeof mem);
        if (l->container[0]) snprintf(mem, sizeof mem, "-");
        if (l->pid >= 0 && !l->container[0]) snprintf(pid, sizeof pid, "%d", l->pid);
        else snprintf(pid, sizeof pid, "-");
        project_of(l, proj, sizeof proj);
        command_of(l, cmd, sizeof cmd);
        if (l->pid < 0 && !l->container[0]) wp_copy(cmd, sizeof cmd, "(another user)");
        app_of(l, app, sizeof app);
        fit(proj, w_proj, cell, sizeof cell);
        fit(cmd, w_cmd, cell2, sizeof cell2);
        fit(app, w_app, cell3, sizeof cell3);
        long long fs = first_seen(seen, l, now_ms, 0);
        int fresh = fs > 0 && now_ms - fs < LIVE_NEW_MS;
        if (r == sel) {
            sb_printf(b, "  %s›%s %s%-6d %-*s  %-*s%-*s  %7s  %9s  %8s%s\033[K\n", CYAN, RESET, C("7"), l->port, w_proj,
                      cell, w_appcol, w_app ? cell3 : "", w_cmd, cell2, pid, up, mem, RESET);
        } else {
            sb_printf(b, "    %s%-6d%s %s%-*s%s  %s%-*s%s%-*s  %s%7s%s  %9s  %8s\033[K\n", fresh ? GREEN : BOLD, l->port,
                      RESET, fresh ? GREEN : is_project(proj) ? CYAN : DIM, w_proj, cell, RESET, MAGENTA, w_appcol,
                      w_app ? cell3 : "", RESET, w_cmd, cell2, DIM, pid, RESET, up, mem);
        }
    }
    if (!nrows) sb_printf(b, "    %sNo listening ports%s.%s\033[K\n", DIM, hidden ? " besides system services (press a)" : "", RESET);
    for (int r = nrows - *scroll; r < body; r++) sb_printf(b, "\033[K\n");

    /* Details of the selected row. */
    sb_printf(b, "\033[K\n");
    int dw = w - 14 > 20 ? w - 14 : 20;
    if (nrows) {
        const listener_t *l = &list->items[rows[sel]];
        project_of(l, proj, sizeof proj);
        char up[32], when[32] = "";
        wp_format_duration(uptime_of(l, now), up, sizeof up);
        if (l->started > 0) {
            struct tm tm;
            wp_localtime(l->started, &tm);
            strftime(when, sizeof when, now - l->started < 86400 ? " since %H:%M" : " since %b %d", &tm);
        }
        if (l->pid < 0 && !l->container[0]) {
            sb_printf(b, "  %sPort %d%s  belongs to another user.\033[K\n", BOLD, l->port, RESET);
            sb_printf(b, "  %sRun whoport %s to see it.%s\033[K\n\033[K\n\033[K\n", DIM,
#ifdef _WIN32
                      "from an administrator terminal",
#else
                      "with sudo",
#endif
                      RESET);
        } else if (l->container[0]) {
            fit(l->container, dw, cell, sizeof cell);
            app_of(l, app, sizeof app);
            sb_printf(b, "  %sPort %d%s  %s%s%s%sDocker container %s%s%s\033[K\n", BOLD, l->port, RESET, MAGENTA, app, RESET,
                      app[0] ? "  " : "", BOLD, cell, RESET);
            fit(l->image, dw, cell, sizeof cell);
            sb_printf(b, "  %sImage%s     %s\033[K\n", DIM, RESET, cell);
        } else {
            app_of(l, app, sizeof app);
            sb_printf(b, "  %sPort %d%s  %s%s%s%s%s %s%s%s %s(pid %d)%s\033[K\n", BOLD, l->port, RESET, MAGENTA, app, RESET,
                      app[0] ? "  " : "", "", BOLD, l->name, RESET, DIM, l->pid, RESET);
            fit(l->command[0] ? l->command : "?", dw, cell, sizeof cell);
            sb_printf(b, "  %sCommand%s   %s\033[K\n", DIM, RESET, cell);
        }
        if (l->pid >= 0 || l->container[0]) {
            fit(proj, dw, cell, sizeof cell);
            sb_printf(b, "  %sProject%s   %s%s%s\033[K\n", DIM, RESET, CYAN, cell, RESET);
            sb_printf(b, "  %sRunning%s   %s%s%s%s  %sAddress%s %s\033[K\n", DIM, RESET, up, DIM, when, RESET, DIM,
                      RESET, l->addr);
        }
    } else {
        sb_printf(b, "\033[K\n\033[K\n\033[K\n\033[K\n");
    }
    sb_printf(b, "\033[K\n  ");
    sb_fit(b, status, w - 3);
    sb_printf(b, "\033[K\n");
    sb_printf(b, "\033[K\n  %s↑↓%s select  %so%s open  %sk%s stop  %sK%s force  %sp%s stop project  %sa%s %s  %sq%s quit\033[K",
              BOLD, RESET, BOLD, RESET, BOLD, RESET, BOLD, RESET, BOLD, RESET, BOLD, RESET, all ? "hide system" : "all",
              BOLD, RESET);
    sb_printf(b, "\033[J");
}

static void live_restore(void) {
    wp_term_raw(0);
    fputs("\033[?25h\033[?1049l", stdout);
    fflush(stdout);
}

static int live_mode(int all) {
    if (!wp_stdout_is_tty() || !wp_stdin_is_tty()) {
        fprintf(stderr, "whoport: --live needs an interactive terminal\n");
        return 2;
    }
    if (wp_term_raw(1) != 0) {
        fprintf(stderr, "whoport: could not switch the terminal to interactive mode\n");
        return 2;
    }
    atexit(live_restore);
    fputs("\033[?1049h\033[?25l\033[2J", stdout);

    listener_list list = {0};
    seen_list seen = {0};
    sbuf out = {0};
    int *rows = NULL, nrows = 0, sel = 0, scroll = 0, sel_port = -1, sel_pid = -1, confirm = 0, initial = 1, force = 0;
    listener_t project_items[64];
    int project_n = 0;
    char project_root[WP_PATH_MAX] = "";
    in_live = 1;
    char status[WP_CMD_MAX + WP_PATH_MAX + 256] = "";
    long long next_refresh = 0, status_until = 0;
    for (;;) {
        long long now_ms = wp_clock_ms();
        if (now_ms >= next_refresh) {
            wp_list_free(&list);
            if (snapshot(&list) != 0) memset(&list, 0, sizeof list);
            int *grown = realloc(rows, (list.len + 1) * sizeof *rows);
            if (grown) rows = grown;
            nrows = 0;
            int hidden = 0;
            for (size_t i = 0; i < list.len; i++) {
                if (find_port(&list, list.items[i].port) != &list.items[i]) continue;
                if (shown(&list.items[i], all)) rows[nrows++] = (int)i;
                else hidden++;
                first_seen(&seen, &list.items[i], now_ms, initial);
            }
            (void)hidden;
            initial = 0;
            /* Keep the selection on the same server when rows move. */
            for (int r = 0; r < nrows; r++)
                if (list.items[rows[r]].port == sel_port && list.items[rows[r]].pid == sel_pid) sel = r;
            if (sel >= nrows) sel = nrows ? nrows - 1 : 0;
            next_refresh = now_ms + 1000;
        }
        if (status_until && now_ms >= status_until && !confirm) {
            status[0] = '\0';
            status_until = 0;
        }
        live_render(&out, &list, rows, nrows, sel, &scroll, all, 0, &seen, status);
        fwrite(out.p, 1, out.len, stdout);
        fflush(stdout);
        if (nrows) {
            sel_port = list.items[rows[sel]].port;
            sel_pid = list.items[rows[sel]].pid;
        }

        long long wait = next_refresh - wp_clock_ms();
        int key = wp_read_key(wait > 0 ? (int)wait : 0);
        if (key == WP_KEY_NONE) continue;
        if (confirm) {
            confirm = 0;
            if ((key == 'y' || key == 'Y') && nrows && project_n) {
                char shown_root[WP_PATH_MAX];
                wp_shorten_home(project_root, home, shown_root, sizeof shown_root);
                snprintf(status, sizeof status, "%sStopping %d server%s in %s...%s", DIM, project_n,
                         project_n == 1 ? "" : "s", shown_root, RESET);
                live_render(&out, &list, rows, nrows, sel, &scroll, all, 0, &seen, status);
                fwrite(out.p, 1, out.len, stdout);
                fflush(stdout);
                int ok = 0;
                char last[sizeof status] = "";
                for (int k = 0; k < project_n; k++) {
                    listener_t victim = project_items[k];
                    if (stop_msg(&victim, 0, last, sizeof last) == 0) ok++;
                }
                if (ok == project_n)
                    snprintf(status, sizeof status, "%s✓%s Stopped %d server%s in %s%s%s.", GREEN, RESET, ok,
                             ok == 1 ? "" : "s", CYAN, shown_root, RESET);
                else
                    snprintf(status, sizeof status, "%sStopped %d of %d. %s", YELLOW, ok, project_n, last);
                project_n = 0;
                next_refresh = 0;
            } else if ((key == 'y' || key == 'Y') && nrows) {
                listener_t victim = list.items[rows[sel]];
                snprintf(status, sizeof status, "%sStopping port %d...%s", DIM, victim.port, RESET);
                live_render(&out, &list, rows, nrows, sel, &scroll, all, 0, &seen, status);
                fwrite(out.p, 1, out.len, stdout);
                fflush(stdout);
                stop_msg(&victim, force, status, sizeof status);
                next_refresh = 0;
            } else {
                status[0] = '\0';
                project_n = 0;
            }
            status_until = wp_clock_ms() + 5000;
            continue;
        }
        switch (key) {
            case 'q': case 'Q': case 3: case WP_KEY_ESC:
                free(rows);
                free(seen.items);
                free(out.p);
                wp_list_free(&list);
                return 0;
            case WP_KEY_UP: if (sel > 0) sel--; break;
            case WP_KEY_DOWN: if (sel + 1 < nrows) sel++; break;
            case WP_KEY_PGUP: sel = sel > 10 ? sel - 10 : 0; break;
            case WP_KEY_PGDN: sel = sel + 10 < nrows ? sel + 10 : (nrows ? nrows - 1 : 0); break;
            case 'a': case 'A': all = !all; next_refresh = 0; break;
            case 'o': case 'O':
                if (nrows) {
                    open_msg(&list.items[rows[sel]], status, sizeof status);
                    status_until = wp_clock_ms() + 5000;
                }
                break;
            case 'p': case 'P':
                if (nrows) {
                    root_of(&list.items[rows[sel]], project_root, sizeof project_root);
                    if (!project_root[0] || is_home_or_root(project_root)) {
                        snprintf(status, sizeof status, "%sPort %d is not part of a project folder.%s", YELLOW,
                                 list.items[rows[sel]].port, RESET);
                        status_until = wp_clock_ms() + 5000;
                        break;
                    }
                    int idx[64], roots;
                    char r[WP_PATH_MAX], shown_root[WP_PATH_MAX];
                    project_n = project_targets(&list, project_root, 0, idx, 64, r, sizeof r, &roots);
                    for (int k = 0; k < project_n; k++) project_items[k] = list.items[idx[k]];
                    wp_shorten_home(project_root, home, shown_root, sizeof shown_root);
                    snprintf(status, sizeof status, "%sStop %s%d server%s%s%s in %s%s%s%s? %sy%s%s/n%s", YELLOW,
                             BOLD, project_n, project_n == 1 ? "" : "s", RESET, YELLOW, CYAN, shown_root, RESET,
                             YELLOW, BOLD, RESET, YELLOW, RESET);
                    confirm = 1;
                }
                break;
            case 'k': case 'K': case 'x':
                if (nrows) {
                    const listener_t *l = &list.items[rows[sel]];
                    char what[WP_CMD_MAX];
                    if (l->container[0]) snprintf(what, sizeof what, "container %s", l->container);
                    else wp_short_command(l->command[0] ? l->command : l->name, home, what, sizeof what);
                    snprintf(status, sizeof status, "%s%s %s%s%s%s on port %d? %sy%s%s/n%s", YELLOW, key == 'K' ? "Force stop" : "Stop", BOLD, what, RESET,
                             YELLOW, l->port, BOLD, RESET, YELLOW, RESET);
                    confirm = 1;
                    force = key == 'K';
                }
                break;
            default: break;
        }
        if (nrows) {
            sel_port = list.items[rows[sel]].port;
            sel_pid = list.items[rows[sel]].pid;
        }
    }
}

int main(int argc, char **argv) {
    int ports[MAX_QUERY], nports = 0;
    int do_kill = 0, force = 0, json = 0, all = 0, free_mode = 0, live = 0, watch = 0, wait = 0, timeout = 60;
    int open_mode = 0, stop_proj = 0, yes = 0;
    const char *project_arg = NULL;
    wp_platform_init();
    color = wp_stdout_is_tty() && !getenv("NO_COLOR");

    /* Subcommands: whoport open 3000, whoport stop [project|port], whoport kill 3000. */
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "open")) {
        open_mode = 1;
        first = 2;
    } else if (argc > 1 && !strcmp(argv[1], "kill")) {
        do_kill = 1;
        first = 2;
    } else if (argc > 1 && !strcmp(argv[1], "stop")) {
        first = 2;
        const char *next = argc > 2 ? argv[2] : NULL;
        if (next && wp_parse_port(next[0] == ':' ? next + 1 : next) > 0) {
            do_kill = 1; /* whoport stop 3000 */
        } else {
            stop_proj = 1;
            if (next && next[0] != '-') {
                project_arg = next;
                first = 3;
            }
        }
    }

    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help") || !strcmp(a, "help") || !strcmp(a, "-?") || !strcmp(a, "/?")) {
            usage(stdout);
            return 0;
        } else if (!strcmp(a, "-v") || !strcmp(a, "--version") || !strcmp(a, "version")) {
            printf("whoport %s\n", WHOPORT_VERSION);
            return 0;
        } else if (!strcmp(a, "-k") || !strcmp(a, "--kill")) {
            do_kill = 1;
        } else if (!strcmp(a, "-f") || !strcmp(a, "--force")) {
            force = 1;
        } else if (!strcmp(a, "--free")) {
            free_mode = 1;
        } else if (!strcmp(a, "-l") || !strcmp(a, "--live")) {
            live = 1;
        } else if (!strcmp(a, "-w") || !strcmp(a, "--watch")) {
            watch = 1;
        } else if (!strcmp(a, "--wait")) {
            wait = 1;
        } else if (!strcmp(a, "-o") || !strcmp(a, "--open")) {
            open_mode = 1;
        } else if (!strcmp(a, "-y") || !strcmp(a, "--yes")) {
            yes = 1;
        } else if (!strcmp(a, "--timeout")) {
            char *end = NULL;
            long t = i + 1 < argc ? strtol(argv[i + 1], &end, 10) : -1;
            if (i + 1 >= argc || !end || *end || t < 0 || t > 86400) {
                fprintf(stderr, "whoport: --timeout needs a number of seconds (0 = wait forever)\n");
                return 2;
            }
            timeout = (int)t;
            i++;
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
    if (live + watch + wait + free_mode + do_kill + open_mode + stop_proj > 1) {
        fprintf(stderr, "whoport: use only one of --live, --watch, --wait, --free, --kill, open and stop\n");
        return 2;
    }
    if (open_mode && !nports) {
        fprintf(stderr, "whoport: open needs a port, e.g. whoport open 3000\n");
        return 2;
    }
    if (stop_proj && nports) {
        fprintf(stderr, "whoport: stop takes a project or a port, not both\n");
        return 2;
    }
    if (json && (live || watch || wait || open_mode || stop_proj)) {
        fprintf(stderr, "whoport: --json does not work with --live, --watch, --wait, open or stop\n");
        return 2;
    }
    if (wait && !nports) {
        fprintf(stderr, "whoport: --wait needs a port, e.g. whoport --wait 5432\n");
        return 2;
    }
    if (live && nports) {
        fprintf(stderr, "whoport: --live shows every port; leave out the port numbers\n");
        return 2;
    }
    if (json) color = 0;
    if (free_mode && (do_kill || nports > 1)) {
        fprintf(stderr, "whoport: --free takes at most one start port, e.g. whoport --free 3000\n");
        return 2;
    }

    home = wp_home();
    if (live) return live_mode(all);
    if (watch) return watch_mode(ports, nports, all);
    if (wait) return wait_mode(ports, nports, timeout);

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

    attach_containers(&list);

    if (stop_proj) {
        int r = stop_project(&list, project_arg, force, yes);
        wp_list_free(&list);
        return r;
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
    } else if (open_mode) {
        for (size_t i = 0; i < picked.len; i++) {
            /* Several entries for one port (IPv4 and IPv6): open it once. */
            if (i && picked.items[i].port == picked.items[i - 1].port) continue;
            char msg[256];
            int r = open_msg(&picked.items[i], msg, sizeof msg);
            fprintf(r ? stderr : stdout, "\n  %s\n", msg);
            if (r) status = r;
        }
        printf("\n");
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
