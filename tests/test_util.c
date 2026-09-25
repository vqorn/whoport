/* Unit tests for util.c. Built with AddressSanitizer and UBSan by `make test`. */
#define _DEFAULT_SOURCE
#include "whoport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

    wp_short_command("node /home/ana/shop/node_modules/.bin/vite --port 5173", NULL, b, sizeof b);
    CHECK_STR(b, "node vite --port 5173");
    wp_short_command("/usr/bin/python3 -m http.server 8000", NULL, b, sizeof b);
    CHECK_STR(b, "python3 -m http.server 8000");
    wp_short_command("npm run dev", NULL, b, sizeof b);
    CHECK_STR(b, "npm run dev");
    wp_short_command("a/", NULL, b, sizeof b);
    CHECK_STR(b, "a/");
    char tiny[6];
    wp_short_command("postgres -D /var/lib/data", NULL, tiny, sizeof tiny);
    CHECK_STR(tiny, "postg");
}

static void test_project_root(void) {
    char base[] = "/tmp/whoport-test-XXXXXX";
    if (!mkdtemp(base)) {
        perror("mkdtemp");
        failures++;
        return;
    }
    char proj[512], deep[512], git[512], out[512];
    snprintf(proj, sizeof proj, "%s/shop", base);
    snprintf(deep, sizeof deep, "%s/shop/src/server", base);
    snprintf(git, sizeof git, "%s/shop/.git", base);
    mkdir(proj, 0700);
    mkdir(git, 0700);
    char mid[512];
    snprintf(mid, sizeof mid, "%s/shop/src", base);
    mkdir(mid, 0700);
    mkdir(deep, 0700);

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

int main(void) {
    test_format();
    test_paths();
    test_project_root();
    test_parse();
    test_dedupe();
    printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
