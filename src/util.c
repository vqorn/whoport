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
#include <strings.h>
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

/* "/usr", "~/code", "C:/code" */
int wp_is_path(const char *s) {
    if (s[0] == '/' || s[0] == '~') return 1;
    return ((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z')) && s[1] == ':' && s[2] == '/';
}

/* Folders that belong to the operating system, not to a project. */
int wp_is_system_dir(const char *dir) {
    if (!dir[0] || strcmp(dir, "/") == 0) return 1;
    if (wp_is_path(dir) && dir[1] == ':' && dir[3] == '\0') return 1; /* "C:/" */
    if (dir[1] == ':' && dir[2] == '/') {
        const char *rest = dir + 3;
        if (strncasecmp(rest, "Windows", 7) == 0 && (rest[7] == '/' || rest[7] == '\0')) return 1;
    }
    return 0;
}

/* Operating system services that listen on ports but are never what you are
 * looking for. Hidden unless --all. */
static const char *const NOISE[] = {
    /* Windows */
    "System", "svchost", "lsass", "wininit", "services", "spoolsv", "csrss", "smss",
    /* macOS */
    "launchd", "rapportd", "ControlCenter", "sharingd", "remoted", "mDNSResponder",
    /* Linux */
    "systemd", "systemd-resolved", "systemd-resolve", "rpcbind", "avahi-daemon", "cupsd", "dnsmasq",
    NULL,
};

int wp_is_os_noise(const listener_t *l) {
    if (l->container[0]) return 0;
    for (int i = 0; NOISE[i]; i++)
        if (strcasecmp(l->name, NOISE[i]) == 0) return 1;
    return 0;
}

typedef struct {
    const char *needle; /* matched as a whole word, case-insensitive */
    const char *name;
    int http;
} app_rule;

/* Docker images. */
static const app_rule IMAGE_RULES[] = {
    {"pgvector", "PostgreSQL", 0}, {"postgis", "PostgreSQL", 0}, {"timescaledb", "PostgreSQL", 0},
    {"postgres", "PostgreSQL", 0}, {"redis", "Redis", 0}, {"valkey", "Valkey", 0}, {"mysql", "MySQL", 0},
    {"mariadb", "MariaDB", 0}, {"mongo", "MongoDB", 0}, {"memcached", "Memcached", 0}, {"rabbitmq", "RabbitMQ", 1},
    {"kafka", "Kafka", 0}, {"minio", "MinIO", 1}, {"elasticsearch", "Elasticsearch", 1},
    {"opensearch", "OpenSearch", 1}, {"clickhouse", "ClickHouse", 1}, {"qdrant", "Qdrant", 1},
    {"meilisearch", "Meilisearch", 1}, {"mailpit", "Mailpit", 1}, {"mailhog", "MailHog", 1},
    {"localstack", "LocalStack", 1}, {"grafana", "Grafana", 1}, {"prometheus", "Prometheus", 1},
    {"keycloak", "Keycloak", 1}, {"adminer", "Adminer", 1}, {"pgadmin4", "pgAdmin", 1}, {"n8n", "n8n", 1},
    {"ollama", "Ollama", 1}, {"text-embeddings-inference", "Embeddings", 1}, {"nginx", "nginx", 1},
    {"httpd", "Apache", 1}, {"traefik", "Traefik", 1}, {"caddy", "Caddy", 1}, {"wordpress", "WordPress", 1},
    {"jupyter", "Jupyter", 1}, {"node", "Node.js", 1}, {"python", "Python", -1},
    {NULL, NULL, 0},
};

/* Command lines and process names, most specific first. */
static const app_rule COMMAND_RULES[] = {
    {"next-server", "Next.js", 1}, {"next dev", "Next.js", 1}, {"next start", "Next.js", 1}, {"nuxt", "Nuxt", 1},
    {"nuxi", "Nuxt", 1}, {"astro", "Astro", 1}, {"remix", "Remix", 1}, {"svelte-kit", "SvelteKit", 1},
    {"storybook", "Storybook", 1}, {"vite", "Vite", 1}, {"react-scripts", "Create React App", 1},
    {"ng serve", "Angular", 1}, {"webpack", "webpack", 1}, {"gatsby", "Gatsby", 1}, {"docusaurus", "Docusaurus", 1},
    {"parcel", "Parcel", 1}, {"expo", "Expo", 1}, {"nest start", "NestJS", 1}, {"wrangler", "Wrangler", 1},
    {"vercel dev", "Vercel", 1}, {"netlify dev", "Netlify", 1}, {"firebase", "Firebase", 1},
    {"supabase", "Supabase", 1}, {"runserver", "Django", 1}, {"uvicorn", "Uvicorn", 1},
    {"gunicorn", "Gunicorn", 1}, {"hypercorn", "Hypercorn", 1}, {"flask", "Flask", 1},
    {"streamlit", "Streamlit", 1}, {"gradio", "Gradio", 1}, {"jupyter-lab", "Jupyter", 1},
    {"jupyter-notebook", "Jupyter", 1}, {"jupyter", "Jupyter", 1}, {"http.server", "http.server", 1},
    {"rails server", "Rails", 1}, {"puma", "Puma", 1}, {"artisan serve", "Laravel", 1}, {"php -s", "PHP", 1},
    {"hugo", "Hugo", 1}, {"jekyll", "Jekyll", 1}, {"spring-boot", "Spring Boot", 1}, {"quarkus", "Quarkus", 1},
    {"code-server", "code-server", 1}, {"ollama", "Ollama", 1}, {"lm studio", "LM Studio", 1},
    {"postgres", "PostgreSQL", 0}, {"postmaster", "PostgreSQL", 0}, {"redis-server", "Redis", 0},
    {"valkey-server", "Valkey", 0}, {"mysqld", "MySQL", 0}, {"mariadbd", "MariaDB", 0}, {"mongod", "MongoDB", 0},
    {"memcached", "Memcached", 0}, {"nginx", "nginx", 1}, {"httpd", "Apache", 1}, {"caddy", "Caddy", 1},
    {"deno", "Deno", 1}, {"bun", "Bun", 1}, {"nodemon", "Node.js", 1}, {"node", "Node.js", 1},
    {"npm", "Node.js", 1}, {"pnpm", "Node.js", 1}, {"yarn", "Node.js", 1}, {"java", "Java", -1},
    {"dotnet", ".NET", 1}, {"python", "Python", -1}, {"python3", "Python", -1}, {"ruby", "Ruby", -1},
    {"php", "PHP", 1},
    {NULL, NULL, 0},
};

static int word_char(char c) {
    return isalnum((unsigned char)c) || c == '_';
}

/* Case-insensitive search for needle as a whole word ("vite" matches
 * "node_modules/.bin/vite --port 5173" but not "vitest"). */
static int has_word(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) {
        if (strncasecmp(p, needle, n) != 0) continue;
        if (p > hay && word_char(p[-1])) continue;
        if (word_char(p[n])) continue;
        return 1;
    }
    return 0;
}

int wp_detect_app(const listener_t *l, char *out, size_t size) {
    out[0] = '\0';
    if (l->container[0]) {
        /* "ghcr.io/org/postgres:16" -> look at the name, not the registry. */
        const char *image = l->image, *slash = strrchr(image, '/');
        char name[128];
        wp_copy(name, sizeof name, slash ? slash + 1 : image);
        char *colon = strchr(name, ':');
        if (colon) *colon = '\0';
        for (const app_rule *r = IMAGE_RULES; r->needle; r++)
            if (has_word(name, r->needle)) {
                wp_copy(out, size, r->name);
                return r->http;
            }
        /* Own images ("myapp-api"): look at what runs inside instead. */
        for (const app_rule *r = COMMAND_RULES; r->needle; r++)
            if (has_word(l->ccommand, r->needle)) {
                wp_copy(out, size, r->name);
                return r->http;
            }
        return -1;
    }
    char hay[WP_CMD_MAX + 80];
    snprintf(hay, sizeof hay, "%s %s", l->command, l->name);
    for (const app_rule *r = COMMAND_RULES; r->needle; r++)
        if (has_word(hay, r->needle)) {
            wp_copy(out, size, r->name);
            return r->http;
        }
    return -1;
}

static int is_wildcard(const char *addr) {
    return strcmp(addr, "0.0.0.0") == 0 || strcmp(addr, "::") == 0 || strcmp(addr, "*") == 0;
}

int wp_is_exposed(const char *addr) {
    if (!addr[0] || wp_is_loopback(addr)) return 0;
    if (!strncmp(addr, "fe80:", 5)) return 0; /* link-local */
    return 1;
}

int wp_is_loopback(const char *addr) {
    return strncmp(addr, "127.", 4) == 0 || strcmp(addr, "::1") == 0 || strcmp(addr, "localhost") == 0 ||
           strncmp(addr, "::ffff:127.", 11) == 0;
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
    /* Windows paths ("C:\Users\ana\shop", e.g. from Docker Compose) are
     * shown with forward slashes and compared case-insensitively. */
    char norm[WP_PATH_MAX];
    int windows = isalpha((unsigned char)path[0]) && path[1] == ':';
    wp_copy(norm, sizeof norm, path);
    if (windows)
        for (char *c = norm; *c; c++)
            if (*c == '\\') *c = '/';
    size_t n = home ? strlen(home) : 0;
    int match = n > 1 && (windows ? strncasecmp(norm, home, n) : strncmp(norm, home, n)) == 0;
    if (match && (norm[n] == '/' || norm[n] == '\0')) {
        snprintf(out, size, "~%s", norm + n);
    } else {
        snprintf(out, size, "%s", norm);
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
