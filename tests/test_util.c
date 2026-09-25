/* Unit tests for util.c. Built with AddressSanitizer and UBSan by `make test`. */
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE /* mkdtemp on macOS */
#include "whoport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
static char *make_temp_dir(char *tmpl) {
    return _mktemp(tmpl) && _mkdir(tmpl) == 0 ? tmpl : NULL;
}
#else
#define MKDIR(p) mkdir((p), 0700)
static char *make_temp_dir(char *tmpl) {
    return mkdtemp(tmpl);
}
#endif

static int failures = 0, checks = 0;

#define CHECK_STR(got, want)                                                                  \
    do {                                                                                      \
        checks++;                                                                             \
        if (strcmp((got), (want)) != 0) {                                                     \
            failures++;                                                                       \
            fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, (got), (want)); \
        }                                                                                     \
    } while (0)

#define CHECK_INT(got, want)                                                                  \
    do {                                                                                      \
        checks++;                                                                             \
        long long g_ = (long long)(got), w_ = (long long)(want);                              \
        if (g_ != w_) {                                                                       \
            failures++;                                                                       \
            fprintf(stderr, "%s:%d: got %lld, want %lld\n", __FILE__, __LINE__, g_, w_);      \
        }                                                                                     \
    } while (0)

static void test_format(void) {
    char b[32];
    wp_format_duration(42, b, sizeof b);            CHECK_STR(b, "42s");
    wp_format_duration(12 * 60 + 5, b, sizeof b);   CHECK_STR(b, "12m");
    wp_format_duration(2 * 3600 + 14 * 60, b, sizeof b); CHECK_STR(b, "2h 14m");
    wp_format_duration(6 * 86400 + 3 * 3600, b, sizeof b); CHECK_STR(b, "6d 3h");
    wp_format_duration(-1, b, sizeof b);            CHECK_STR(b, "?");

    wp_format_bytes(900, b, sizeof b);                   CHECK_STR(b, "1 KB");
    wp_format_bytes(182LL * 1024 * 1024, b, sizeof b);   CHECK_STR(b, "182 MB");
    wp_format_bytes(3LL * 1024 * 1024 * 1024 / 2, b, sizeof b); CHECK_STR(b, "1.5 GB");
    wp_format_bytes(-1, b, sizeof b);                    CHECK_STR(b, "?");
}

static void test_paths(void) {
    char b[256];
    wp_shorten_home("/home/ana/code/shop", "/home/ana", b, sizeof b); CHECK_STR(b, "~/code/shop");
    wp_shorten_home("/home/ana", "/home/ana", b, sizeof b);           CHECK_STR(b, "~");
    wp_shorten_home("/home/anabel/x", "/home/ana", b, sizeof b);      CHECK_STR(b, "/home/anabel/x");
    wp_shorten_home("/srv/app", NULL, b, sizeof b);                   CHECK_STR(b, "/srv/app");
    wp_shorten_home("C:\\Users\\johan\\Fontia", "C:/Users/johan", b, sizeof b); CHECK_STR(b, "~/Fontia");
    wp_shorten_home("c:\\users\\johan\\x", "C:/Users/johan", b, sizeof b);      CHECK_STR(b, "~/x");
    wp_shorten_home("D:\\code\\shop", "C:/Users/johan", b, sizeof b);         CHECK_STR(b, "D:/code/shop");

    wp_short_command("node /home/ana/shop/node_modules/.bin/vite --port 5173", NULL, b, sizeof b);
    CHECK_STR(b, "node vite --port 5173");
    wp_short_command("/usr/bin/python3 -m http.server 8000", NULL, b, sizeof b);
    CHECK_STR(b, "python3 -m http.server 8000");
    wp_short_command("\"C:\\Program Files\\nodejs\\node.exe\" server.js", NULL, b, sizeof b);
    CHECK_STR(b, "node server.js");
    wp_short_command("\"unterminated", NULL, b, sizeof b);
    CHECK_STR(b, "unterminated");
    wp_short_command("C:\\nodejs\\node.exe D:\\code\\shop\\server.js", NULL, b, sizeof b);
    CHECK_STR(b, "node server.js");
    wp_short_command("npm run dev", NULL, b, sizeof b);
    CHECK_STR(b, "npm run dev");
    wp_short_command("a/", NULL, b, sizeof b);
    CHECK_STR(b, "a/");
    char tiny[6];
    wp_short_command("postgres -D /var/lib/data", NULL, tiny, sizeof tiny);
    CHECK_STR(tiny, "postg");
}

static void test_system_dirs(void) {
    CHECK_INT(wp_is_system_dir(""), 1);
    CHECK_INT(wp_is_system_dir("/"), 1);
    CHECK_INT(wp_is_system_dir("C:/"), 1);
    CHECK_INT(wp_is_system_dir("C:/Windows/system32"), 1);
    CHECK_INT(wp_is_system_dir("c:/windows"), 1);
    CHECK_INT(wp_is_system_dir("C:/WindowsApps"), 0);
    CHECK_INT(wp_is_system_dir("D:/code/shop"), 0);
    CHECK_INT(wp_is_system_dir("/home/ana/shop"), 0);
    CHECK_INT(wp_is_path("~/code"), 1);
    CHECK_INT(wp_is_path("D:/a/b"), 1);
    CHECK_INT(wp_is_path("node server.js"), 0);
}

static void test_project_root(void) {
#ifdef _WIN32
    char base[512];
    const char *tmp = getenv("TEMP");
    snprintf(base, sizeof base, "%s/whoport-test-XXXXXX", tmp ? tmp : ".");
    for (char *c = base; *c; c++)
        if (*c == '\\') *c = '/';
#else
    char base[] = "/tmp/whoport-test-XXXXXX";
#endif
    if (!make_temp_dir(base)) {
        perror("mkdtemp");
        failures++;
        return;
    }
    char proj[512], deep[512], git[512], out[512];
    snprintf(proj, sizeof proj, "%s/shop", base);
    snprintf(deep, sizeof deep, "%s/shop/src/server", base);
    snprintf(git, sizeof git, "%s/shop/.git", base);
    MKDIR(proj);
    MKDIR(git);
    char mid[512];
    snprintf(mid, sizeof mid, "%s/shop/src", base);
    MKDIR(mid);
    MKDIR(deep);

    CHECK_INT(wp_find_project_root(deep, base, out, sizeof out), 1);
    CHECK_STR(out, proj);
    /* Never climbs above home: from base itself nothing is found. */
    CHECK_INT(wp_find_project_root(base, base, out, sizeof out), 0);
    CHECK_STR(out, base);
    CHECK_INT(wp_find_project_root("/", NULL, out, sizeof out), 0);

    rmdir(deep);
    rmdir(mid);
    rmdir(git);
    rmdir(proj);
    rmdir(base);
}

static void test_parse(void) {
    CHECK_INT(wp_parse_port("3000"), 3000);
    CHECK_INT(wp_parse_port("65535"), 65535);
    CHECK_INT(wp_parse_port("65536"), -1);
    CHECK_INT(wp_parse_port("0"), -1);
    CHECK_INT(wp_parse_port("30a0"), -1);
    CHECK_INT(wp_parse_port(""), -1);

#ifndef _WIN32
    int port, state;
    unsigned long inode;
    char addr[64];
    const char *v4 = "   0: 0100007F:0BB8 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 48213 1 0000000000000000 100 0 0 10 0";
    CHECK_INT(wp_parse_proc_net_line(v4, 0, &port, addr, sizeof addr, &inode, &state), 1);
    CHECK_INT(port, 3000);
    CHECK_STR(addr, "127.0.0.1");
    CHECK_INT(inode, 48213);
    CHECK_INT(state, 0x0A);

    const char *any = "   1: 00000000:1538 00000000:0000 0A 00000000:00000000 00:00000000 00000000     0        0 9911 1 0000000000000000 100 0 0 10 0";
    CHECK_INT(wp_parse_proc_net_line(any, 0, &port, addr, sizeof addr, &inode, &state), 1);
    CHECK_INT(port, 5432);
    CHECK_STR(addr, "0.0.0.0");

    const char *v6 = "   0: 00000000000000000000000001000000:1435 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 77001 1 0000000000000000 100 0 0 10 0";
    CHECK_INT(wp_parse_proc_net_line(v6, 1, &port, addr, sizeof addr, &inode, &state), 1);
    CHECK_INT(port, 5173);
    CHECK_STR(addr, "::1");

    const char *mapped = "   0: 0000000000000000FFFF00000100007F:1F90 00000000000000000000000000000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 5 1 0000000000000000 100 0 0 10 0";
    CHECK_INT(wp_parse_proc_net_line(mapped, 1, &port, addr, sizeof addr, &inode, &state), 1);
    CHECK_STR(addr, "127.0.0.1");

    CHECK_INT(wp_parse_proc_net_line("  sl  local_address rem_address   st", 0, &port, addr, sizeof addr, &inode, &state), 0);
    CHECK_INT(wp_parse_proc_net_line(v6, 0, &port, addr, sizeof addr, &inode, &state), 0);
#endif
}

static void test_dedupe(void) {
    listener_list list = {0};
    listener_t *a = wp_list_push(&list);
    a->port = 8080; a->pid = 7; wp_copy(a->addr, sizeof a->addr, "127.0.0.1");
    listener_t *b = wp_list_push(&list);
    b->port = 3000; b->pid = 9; wp_copy(b->addr, sizeof b->addr, "0.0.0.0");
    listener_t *c = wp_list_push(&list);
    c->port = 8080; c->pid = 7; c->ipv6 = 1; wp_copy(c->addr, sizeof c->addr, "::1");
    listener_t *d = wp_list_push(&list);
    d->port = 3000; d->pid = 9; d->ipv6 = 1; wp_copy(d->addr, sizeof d->addr, "::");
    for (int i = 0; i < 100; i++) { /* force the list to grow */
        listener_t *x = wp_list_push(&list);
        x->port = 10000 + i; x->pid = 100 + i; wp_copy(x->addr, sizeof x->addr, "0.0.0.0");
    }
    wp_list_sort_dedupe(&list);
    CHECK_INT(list.len, 102);
    CHECK_INT(list.items[0].port, 3000);
    CHECK_STR(list.items[0].addr, "0.0.0.0");
    CHECK_INT(list.items[1].port, 8080);
    CHECK_STR(list.items[1].addr, "localhost");
    CHECK_INT(list.items[1].pid, 7);
    wp_list_free(&list);

    char buf[4];
    wp_copy(buf, sizeof buf, "abcdef");
    CHECK_STR(buf, "abc");
}

static void test_docker(void) {
    /* Trimmed real /containers/json output: a Compose service with two port
     * bindings (IPv4 + IPv6), a plain container, and one without ports. */
    const char *json =
        "[{\"Id\":\"8dfafdbc3a40\",\"Names\":[\"/shop-db-1\"],\"Image\":\"postgres:16\",\"Command\":\"docker-entrypoint.sh postgres\","
        "\"Created\":1727000000,\"Ports\":[{\"IP\":\"0.0.0.0\",\"PrivatePort\":5432,\"PublicPort\":5433,\"Type\":\"tcp\"},"
        "{\"IP\":\"::\",\"PrivatePort\":5432,\"PublicPort\":5433,\"Type\":\"tcp\"}],"
        "\"Labels\":{\"com.docker.compose.project\":\"shop\",\"com.docker.compose.project.working_dir\":\"/home/ana/code/shop\","
        "\"com.docker.compose.service\":\"db\",\"note\":\"a \\\"quoted\\\" \\u00e9 value\"},\"State\":\"running\","
        "\"HostConfig\":{\"NetworkMode\":\"shop_default\"},\"NetworkSettings\":{\"Networks\":{\"x\":{\"IPAddress\":\"172.18.0.2\"}}},"
        "\"Mounts\":[]},"
        "{\"Id\":\"a1\",\"Names\":[\"/redis\"],\"Image\":\"redis:7\",\"Ports\":[{\"PrivatePort\":6379,\"PublicPort\":6379,\"Type\":\"tcp\"},"
        "{\"PrivatePort\":6379,\"PublicPort\":6379,\"Type\":\"udp\"},{\"PrivatePort\":9999,\"Type\":\"tcp\"}],\"Labels\":null},"
        "{\"Id\":\"b2\",\"Names\":[\"/worker\"],\"Image\":\"busybox\",\"Ports\":[],\"Labels\":{}}]";
    wp_container *c = NULL;
    size_t n = 0;
    CHECK_INT(wp_docker_parse(json, strlen(json), &c, &n), 0);
    CHECK_INT(n, 2);
    if (n == 2) {
        CHECK_INT(c[0].port, 5433);
        CHECK_STR(c[0].name, "shop-db-1");
        CHECK_STR(c[0].image, "postgres:16");
        CHECK_STR(c[0].workdir, "/home/ana/code/shop");
        CHECK_STR(c[0].service, "db");
        CHECK_INT(c[0].created, 1727000000);
        CHECK_INT(c[1].port, 6379);
        CHECK_STR(c[1].name, "redis");
        CHECK_STR(c[1].workdir, "");
    }
    free(c);
    /* Docker 29 sends null for containers without published ports. */
    const char *nulls =
        "[{\"Id\":\"c3\",\"Names\":null,\"Image\":null,\"Ports\":null,\"Labels\":null,\"Mounts\":null},"
        "{\"Id\":\"d4\",\"Names\":[\"/api\"],\"Image\":\"node:22\",\"Ports\":[null,{\"PublicPort\":null,\"Type\":\"tcp\"},"
        "{\"IP\":\"::\",\"PrivatePort\":80,\"PublicPort\":8080,\"Type\":\"tcp\"}],"
        "\"Labels\":{\"com.docker.compose.project.working_dir\":null,\"x\":\"y\"}}]";
    CHECK_INT(wp_docker_parse(nulls, strlen(nulls), &c, &n), 0);
    CHECK_INT(n, 1);
    if (n == 1) {
        CHECK_INT(c[0].port, 8080);
        CHECK_STR(c[0].name, "api");
        CHECK_STR(c[0].image, "node:22");
        CHECK_STR(c[0].workdir, "");
    }
    free(c);
    CHECK_INT(wp_parse_rfc3339("1970-01-02T00:00:00Z"), -1); /* before 1971: treated as unknown */
    CHECK_INT(wp_parse_rfc3339("2024-09-22T10:13:20Z"), 1727000000);
    CHECK_INT(wp_parse_rfc3339("2024-09-22T10:13:20.123456789Z"), 1727000000);
    CHECK_INT(wp_parse_rfc3339("2024-09-22T12:13:20+02:00"), 1727000000);
    CHECK_INT(wp_parse_rfc3339("2024-02-29T00:00:00Z"), 1709164800);
    CHECK_INT(wp_parse_rfc3339("0001-01-01T00:00:00Z"), -1);
    CHECK_INT(wp_parse_rfc3339("yesterday"), -1);
    CHECK_INT(wp_docker_started_at("{\"State\":{\"Status\":\"running\",\"StartedAt\":\"2024-09-22T10:13:20.5Z\"}}"), 1727000000);
    CHECK_INT(wp_docker_started_at("{\"State\":{}}"), -1);
    CHECK_INT(wp_docker_parse("[]", 2, &c, &n), 0);
    CHECK_INT(n, 0);
    CHECK_INT(wp_docker_parse("[{\"Names\":[\"/x\"", 15, &c, &n), -1);
    CHECK_INT(wp_docker_parse("{\"message\":\"no\"}", 16, &c, &n), -1);

    char r1[] = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n[]";
    char *body;
    size_t blen;
    CHECK_INT(wp_http_parse(r1, strlen(r1), &body, &blen), 200);
    CHECK_INT(blen, 2);
    char r2[] = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\n[{}\r\n1\r\n]\r\n0\r\n\r\n";
    CHECK_INT(wp_http_parse(r2, strlen(r2), &body, &blen), 200);
    CHECK_INT(blen, 4);
    body[blen] = '\0';
    CHECK_STR(body, "[{}]");
    char r3[] = "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\n[]";
    CHECK_INT(wp_http_parse(r3, strlen(r3), &body, &blen), -1); /* incomplete */
    char r4[] = "HTTP/1.1 204 No Content\r\n\r\n";
    CHECK_INT(wp_http_parse(r4, strlen(r4), &body, &blen), 204);
}

static void test_http_complete(void) {
    /* Content-Length: complete once the body is all there. */
    const char *cl = "HTTP/1.0 200 OK\r\nContent-Length: 4\r\n\r\n[{}]";
    CHECK_INT(wp_http_complete(cl, strlen(cl)), 1);
    CHECK_INT(wp_http_complete(cl, strlen(cl) - 2), 0);
    /* No Content-Length (Docker, large response to HTTP/1.0): only the peer
     * closing the connection ends it, so headers alone are never complete. */
    const char *open_ended = "HTTP/1.0 200 OK\r\nApi-Version: 1.47\r\nContent-Type: application/json\r\n\r\n[{\"Id\":";
    CHECK_INT(wp_http_complete(open_ended, strlen(open_ended)), 0);
    const char *headers_only = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n";
    CHECK_INT(wp_http_complete(headers_only, strlen(headers_only)), 0);
    /* Chunked: complete at the last chunk. */
    const char *ch = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n[]\r\n0\r\n\r\n";
    CHECK_INT(wp_http_complete(ch, strlen(ch)), 1);
    CHECK_INT(wp_http_complete(ch, strlen(ch) - 5), 0);
    /* The body is still parsed from what arrived before the close. */
    char resp[] = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n[]";
    char *body;
    size_t blen;
    CHECK_INT(wp_http_parse(resp, strlen(resp), &body, &blen), 200);
    CHECK_INT(blen, 2);
}

static void test_noise(void) {
    listener_t l;
    memset(&l, 0, sizeof l);
    wp_copy(l.name, sizeof l.name, "svchost");
    CHECK_INT(wp_is_os_noise(&l), 1);
    wp_copy(l.name, sizeof l.name, "node");
    CHECK_INT(wp_is_os_noise(&l), 0);
    wp_copy(l.name, sizeof l.name, "System");
    wp_copy(l.container, sizeof l.container, "redis");
    CHECK_INT(wp_is_os_noise(&l), 0); /* a container is never noise */
}

int main(void) {
    test_format();
    test_paths();
    test_system_dirs();
    test_project_root();
    test_parse();
    test_dedupe();
    test_docker();
    test_http_complete();
    test_noise();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
