/* Docker: which container publishes which port, and which Compose project it
 * belongs to. Talks to the Docker Engine API directly over its local socket
 * (unix socket on Linux/macOS, named pipe on Windows); no docker CLI needed. */
#ifndef _WIN32
#define _DEFAULT_SOURCE
#endif
#include "whoport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define MAX_RESPONSE (8 * 1024 * 1024)

/* ---------- Minimal JSON reader (only what /containers/json needs) ---------- */

typedef struct {
    const char *p;
    const char *end;
} js;

static void ws(js *j) {
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r')) j->p++;
}

static int expect(js *j, char c) {
    ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return 1;
    }
    return 0;
}

static void put_utf8(unsigned cp, char *out, size_t *w, size_t size) {
    char buf[4];
    size_t n;
    if (cp < 0x80) { buf[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { buf[0] = (char)(0xC0 | (cp >> 6)); buf[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else { buf[0] = (char)(0xE0 | (cp >> 12)); buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); buf[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    for (size_t i = 0; i < n && *w + 1 < size; i++) out[(*w)++] = buf[i];
}

/* Reads a string into out (may be NULL to skip). Returns 1 on success. */
static int js_string(js *j, char *out, size_t size) {
    ws(j);
    if (j->p >= j->end || *j->p != '"') return 0;
    j->p++;
    size_t w = 0;
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c == '\\' && j->p < j->end) {
            char e = *j->p++;
            unsigned cp = (unsigned char)e;
            switch (e) {
                case 'n': cp = '\n'; break;
                case 't': cp = '\t'; break;
                case 'r': cp = '\r'; break;
                case 'b': cp = '\b'; break;
                case 'f': cp = '\f'; break;
                case 'u': {
                    cp = 0;
                    for (int i = 0; i < 4 && j->p < j->end; i++) {
                        char h = *j->p++;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                    }
                    break;
                }
                default: break; /* \" \\ \/ */
            }
            if (out) put_utf8(cp, out, &w, size);
        } else if (out && w + 1 < size) {
            out[w++] = c;
        }
    }
    if (j->p >= j->end) return 0;
    j->p++; /* closing quote */
    if (out && size) out[w] = '\0';
    return 1;
}

static int js_skip(js *j, int depth);

static int js_skip_container(js *j, char open, char close, int depth) {
    if (!expect(j, open)) return 0;
    if (expect(j, close)) return 1;
    for (;;) {
        if (open == '{') {
            if (!js_string(j, NULL, 0) || !expect(j, ':')) return 0;
        }
        if (!js_skip(j, depth + 1)) return 0;
        if (expect(j, ',')) continue;
        return expect(j, close);
    }
}

/* Skips any JSON value. */
static int js_skip(js *j, int depth) {
    if (depth > 64) return 0;
    ws(j);
    if (j->p >= j->end) return 0;
    char c = *j->p;
    if (c == '"') return js_string(j, NULL, 0);
    if (c == '{') return js_skip_container(j, '{', '}', depth);
    if (c == '[') return js_skip_container(j, '[', ']', depth);
    /* number, true, false, null */
    const char *start = j->p;
    while (j->p < j->end && !strchr(",]} \t\r\n", *j->p)) j->p++;
    return j->p > start;
}

/* Next non-space character, or 0 at the end. Used to skip values of an
 * unexpected type (Docker 29 sends "Ports":null for containers without ports). */
static char peek(js *j) {
    ws(j);
    return j->p < j->end ? *j->p : 0;
}

static int js_number(js *j, long *out) {
    ws(j);
    char *endp;
    long v = strtol(j->p, &endp, 10);
    if (endp == j->p || endp > j->end) return 0;
    *out = v;
    j->p = endp;
    /* Skip a fractional part if there is one. */
    while (j->p < j->end && !strchr(",]} \t\r\n", *j->p)) j->p++;
    return 1;
}

static wp_container *push_container(wp_container **list, size_t *n, size_t *cap) {
    if (*n == *cap) {
        size_t c = *cap ? *cap * 2 : 16;
        wp_container *grown = realloc(*list, c * sizeof *grown);
        if (!grown) return NULL;
        *list = grown;
        *cap = c;
    }
    wp_container *e = &(*list)[(*n)++];
    memset(e, 0, sizeof *e);
    return e;
}

static size_t parse_error_at; /* for WHOPORT_DEBUG */

/* Parses the /containers/json array into one entry per published TCP port. */
int wp_docker_parse(const char *json, size_t len, wp_container **out, size_t *count) {
    js j = {json, json + len};
    size_t n = 0, cap = 0;
    wp_container *list = NULL;
    *out = NULL;
    *count = 0;
    if (!expect(&j, '[')) return -1;
    if (expect(&j, ']')) return 0;
    for (;;) {
        char name[128] = "", image[128] = "", workdir[WP_PATH_MAX] = "", service[128] = "";
        long created = 0;
        long ports[64];
        int nports = 0;
        if (!expect(&j, '{')) goto fail;
        if (!expect(&j, '}')) {
            for (;;) {
                char key[64];
                if (!js_string(&j, key, sizeof key) || !expect(&j, ':')) goto fail;
                if (!strcmp(key, "Names") && peek(&j) == '[') {
                    expect(&j, '[');
                    if (!expect(&j, ']')) {
                        int first = 1;
                        for (;;) {
                            char tmp[128];
                            if (peek(&j) != '"') {
                                if (!js_skip(&j, 0)) goto fail;
                            } else {
                                if (!js_string(&j, tmp, sizeof tmp)) goto fail;
                                if (first) wp_copy(name, sizeof name, tmp[0] == '/' ? tmp + 1 : tmp);
                            }
                            first = 0;
                            if (expect(&j, ',')) continue;
                            if (!expect(&j, ']')) goto fail;
                            break;
                        }
                    }
                } else if (!strcmp(key, "Created") && peek(&j) >= '0' && peek(&j) <= '9') {
                    if (!js_number(&j, &created)) goto fail;
                } else if (!strcmp(key, "Image") && peek(&j) == '"') {
                    if (!js_string(&j, image, sizeof image)) goto fail;
                } else if (!strcmp(key, "Ports") && peek(&j) == '[') {
                    expect(&j, '[');
                    if (!expect(&j, ']')) {
                        for (;;) {
                            long pub = 0;
                            char type[16] = "";
                            if (peek(&j) != '{') {
                                if (!js_skip(&j, 0)) goto fail;
                            } else if (expect(&j, '{') && !expect(&j, '}')) {
                                for (;;) {
                                    char pk[32];
                                    if (!js_string(&j, pk, sizeof pk) || !expect(&j, ':')) goto fail;
                                    char next = peek(&j);
                                    if (!strcmp(pk, "PublicPort") && next >= '0' && next <= '9') {
                                        if (!js_number(&j, &pub)) goto fail;
                                    } else if (!strcmp(pk, "Type") && next == '"') {
                                        if (!js_string(&j, type, sizeof type)) goto fail;
                                    } else if (!js_skip(&j, 0)) {
                                        goto fail;
                                    }
                                    if (expect(&j, ',')) continue;
                                    if (!expect(&j, '}')) goto fail;
                                    break;
                                }
                            }
                            if (pub > 0 && pub <= 65535 && !strcmp(type, "tcp") && nports < 64) {
                                int dup = 0;
                                for (int k = 0; k < nports; k++) dup |= ports[k] == pub;
                                if (!dup) ports[nports++] = pub;
                            }
                            if (expect(&j, ',')) continue;
                            if (!expect(&j, ']')) goto fail;
                            break;
                        }
                    }
                } else if (!strcmp(key, "Labels") && peek(&j) == '{') {
                    {
                        expect(&j, '{');
                        if (!expect(&j, '}')) {
                            for (;;) {
                                char lk[128];
                                if (!js_string(&j, lk, sizeof lk) || !expect(&j, ':')) goto fail;
                                char next = peek(&j);
                                if (!strcmp(lk, "com.docker.compose.project.working_dir") && next == '"') {
                                    if (!js_string(&j, workdir, sizeof workdir)) goto fail;
                                } else if (!strcmp(lk, "com.docker.compose.service") && next == '"') {
                                    if (!js_string(&j, service, sizeof service)) goto fail;
                                } else if (!js_skip(&j, 0)) {
                                    goto fail;
                                }
                                if (expect(&j, ',')) continue;
                                if (!expect(&j, '}')) goto fail;
                                break;
                            }
                        }
                    }
                } else if (!js_skip(&j, 0)) {
                    goto fail;
                }
                if (expect(&j, ',')) continue;
                if (!expect(&j, '}')) goto fail;
                break;
            }
        }
        for (int k = 0; k < nports; k++) {
            wp_container *e = push_container(&list, &n, &cap);
            if (!e) goto fail;
            e->port = (int)ports[k];
            wp_copy(e->name, sizeof e->name, name);
            wp_copy(e->image, sizeof e->image, image);
            wp_copy(e->workdir, sizeof e->workdir, workdir);
            wp_copy(e->service, sizeof e->service, service);
            e->created = created;
        }
        if (expect(&j, ',')) continue;
        if (!expect(&j, ']')) goto fail;
        break;
    }
    *out = list;
    *count = n;
    return 0;
fail:
    free(list);
    parse_error_at = (size_t)(j.p - json);
    return -1;
}

/* ---------- HTTP over a local socket ---------- */

/* Splits a raw HTTP/1.x response. Returns the status code, or -1 when the
 * response is incomplete or malformed. Handles Content-Length and chunked. */
int wp_http_parse(char *resp, size_t len, char **body, size_t *body_len) {
    char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (resp[i] == '\r' && resp[i + 1] == '\n' && resp[i + 2] == '\r' && resp[i + 3] == '\n') {
            hdr_end = resp + i;
            break;
        }
    }
    if (!hdr_end || len < 12 || strncmp(resp, "HTTP/1.", 7) != 0) return -1;
    int status = atoi(resp + 9);
    char *b = hdr_end + 4;
    size_t avail = len - (size_t)(b - resp);
    long content_length = -1;
    int chunked = 0;
    for (char *line = resp; line < hdr_end;) {
        char *eol = strstr(line, "\r\n");
        if (!eol || eol > hdr_end) eol = hdr_end;
        if ((size_t)(eol - line) > 15 && strncasecmp(line, "Content-Length:", 15) == 0) content_length = atol(line + 15);
        if ((size_t)(eol - line) > 18 && strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strstr(line, "chunked") &&
            strstr(line, "chunked") < eol)
            chunked = 1;
        line = eol + 2;
    }
    if (chunked) {
        /* Decode in place. */
        char *r = b, *w = b, *end = b + avail;
        for (;;) {
            char *eol = NULL;
            for (char *q = r; q + 1 < end; q++)
                if (q[0] == '\r' && q[1] == '\n') { eol = q; break; }
            if (!eol) return -1;
            long size = strtol(r, NULL, 16);
            r = eol + 2;
            if (size == 0) break;
            if (size < 0 || r + size > end) return -1;
            memmove(w, r, (size_t)size);
            w += size;
            r += size + 2;
            if (r > end) return -1;
        }
        *body = b;
        *body_len = (size_t)(w - b);
        return status;
    }
    if (content_length >= 0) {
        if ((size_t)content_length > avail) return -1;
        avail = (size_t)content_length;
    }
    *body = b;
    *body_len = avail;
    return status;
}

/* Whether a response can be read to the end without waiting for the peer to
 * close: headers are in and the body reached Content-Length, or the final
 * chunk arrived. Without either, the body ends when the connection closes
 * (Docker leaves out Content-Length for large responses to HTTP/1.0). */
int wp_http_complete(const char *buf, size_t len) {
    const char *hdr_end = NULL;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            hdr_end = buf + i;
            break;
        }
    }
    if (!hdr_end) return 0;
    size_t body = len - (size_t)(hdr_end + 4 - buf);
    for (const char *line = buf; line < hdr_end;) {
        const char *eol = line;
        while (eol < hdr_end && *eol != '\r') eol++;
        size_t n = (size_t)(eol - line);
        if (n > 15 && strncasecmp(line, "Content-Length:", 15) == 0) return body >= (size_t)atol(line + 15);
        if (n > 18 && strncasecmp(line, "Transfer-Encoding:", 18) == 0) {
            for (const char *q = line + 18; q + 7 <= eol; q++)
                if (strncasecmp(q, "chunked", 7) == 0) {
                    /* Last chunk: "0\r\n\r\n" at the very end. */
                    return body >= 5 && memcmp(buf + len - 5, "0\r\n\r\n", 5) == 0;
                }
        }
        line = eol + 2;
    }
    return 0;
}

static int response_complete(char *buf, size_t len) {
    return wp_http_complete(buf, len);
}

#ifdef _WIN32
static HANDLE open_pipe(const char *pipe) {
    HANDLE h = CreateFileA(pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY && WaitNamedPipeA(pipe, 500))
        h = CreateFileA(pipe, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (getenv("WHOPORT_DEBUG"))
        fprintf(stderr, "whoport debug: docker pipe %s: %s (error %lu)\n", pipe,
                h == INVALID_HANDLE_VALUE ? "failed" : "connected",
                h == INVALID_HANDLE_VALUE ? (unsigned long)GetLastError() : 0UL);
    return h;
}

static int docker_request(const char *request, char **resp, size_t *resp_len) {
    /* DOCKER_HOST first, then Docker Desktop's Linux engine (current default),
     * the classic engine pipe, and the older Linux engine name. */
    char pipes[4][256];
    int n = 0;
    const char *host = getenv("DOCKER_HOST");
    if (host && strncmp(host, "npipe://", 8) == 0) {
        wp_copy(pipes[n], sizeof pipes[n], host + 8);
        for (char *c = pipes[n]; *c; c++)
            if (*c == '/') *c = '\\';
        n++;
    }
    wp_copy(pipes[n++], sizeof pipes[0], "\\\\.\\pipe\\dockerDesktopLinuxEngine");
    wp_copy(pipes[n++], sizeof pipes[0], "\\\\.\\pipe\\docker_engine");
    wp_copy(pipes[n++], sizeof pipes[0], "\\\\.\\pipe\\docker_engine_linux");

    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < n && h == INVALID_HANDLE_VALUE; i++) h = open_pipe(pipes[i]);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD written;
    if (!WriteFile(h, request, (DWORD)strlen(request), &written, NULL)) {
        CloseHandle(h);
        return -1;
    }
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (len + 4096 > cap) {
            if (cap >= MAX_RESPONSE) break;
            char *grown = realloc(buf, cap * 2);
            if (!grown) break;
            buf = grown;
            cap *= 2;
        }
        DWORD got = 0;
        if (!ReadFile(h, buf + len, (DWORD)(cap - len - 1), &got, NULL)) {
            DWORD err = GetLastError();
            if (err != ERROR_MORE_DATA) {
                if (getenv("WHOPORT_DEBUG"))
                    fprintf(stderr, "whoport debug: docker pipe read stopped after %zu bytes (error %lu)\n", len,
                            (unsigned long)err);
                break;
            }
        }
        if (got == 0) break;
        len += got;
        if (response_complete(buf, len)) break;
    }
    CloseHandle(h);
    if (!buf) return -1;
    buf[len] = '\0';
    if (getenv("WHOPORT_DEBUG")) fprintf(stderr, "whoport debug: docker answered %zu bytes\n", len);
    *resp = buf;
    *resp_len = len;
    return 0;
}
#else
static int connect_unix(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof addr.sun_path) {
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path, path, strlen(path) + 1);
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int docker_request(const char *request, char **resp, size_t *resp_len) {
    /* DOCKER_HOST first, then the usual places for Docker Engine, Docker
     * Desktop, Colima, OrbStack and rootless Docker. */
    char paths[7][WP_PATH_MAX];
    int n = 0;
    const char *host = getenv("DOCKER_HOST");
    const char *home = wp_home();
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (host && strncmp(host, "unix://", 7) == 0) wp_copy(paths[n++], WP_PATH_MAX, host + 7);
    wp_copy(paths[n++], WP_PATH_MAX, "/var/run/docker.sock");
    if (home) {
        snprintf(paths[n++], WP_PATH_MAX, "%s/.docker/run/docker.sock", home);
        snprintf(paths[n++], WP_PATH_MAX, "%s/.colima/default/docker.sock", home);
        snprintf(paths[n++], WP_PATH_MAX, "%s/.orbstack/run/docker.sock", home);
        snprintf(paths[n++], WP_PATH_MAX, "%s/.docker/desktop/docker.sock", home);
    }
    if (xdg) snprintf(paths[n++], WP_PATH_MAX, "%s/docker.sock", xdg);

    int fd = -1;
    for (int i = 0; i < n && fd < 0; i++) {
        fd = connect_unix(paths[i]);
        if (getenv("WHOPORT_DEBUG"))
            fprintf(stderr, "whoport debug: docker socket %s: %s\n", paths[i], fd < 0 ? "failed" : "connected");
    }
    if (fd < 0) return -1;

    size_t sent = 0, want = strlen(request);
    while (sent < want) {
        ssize_t w = write(fd, request + sent, want - sent);
        if (w <= 0) {
            close(fd);
            return -1;
        }
        sent += (size_t)w;
    }
    size_t cap = 65536, len = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (len + 4096 > cap) {
            if (cap >= MAX_RESPONSE) break;
            char *grown = realloc(buf, cap * 2);
            if (!grown) break;
            buf = grown;
            cap *= 2;
        }
        ssize_t got = read(fd, buf + len, cap - len - 1);
        if (got <= 0) break;
        len += (size_t)got;
        if (response_complete(buf, len)) break;
    }
    close(fd);
    if (!buf) return -1;
    buf[len] = '\0';
    *resp = buf;
    *resp_len = len;
    return 0;
}
#endif

/* WHOPORT_DEBUG: show the start of a response we could not use, escaped. */
static void debug_dump_raw(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\r') fputs("\\r", stderr);
        else if (c == '\n') fputs("\\n\n  ", stderr);
        else if (c < 32 || c > 126) fprintf(stderr, "\\x%02x", c);
        else fputc(c, stderr);
    }
    fputc('\n', stderr);
}

static void debug_dump(const char *buf, size_t len) {
    fprintf(stderr, "whoport debug: response starts with:\n  ");
    debug_dump_raw(buf, len < 600 ? len : 600);
}

static void fill_started(wp_container *list, size_t n);

int wp_docker_containers(wp_container **out, size_t *count) {
    *out = NULL;
    *count = 0;
    if (getenv("WHOPORT_NO_DOCKER")) return -1;
    char *resp;
    size_t len;
    if (docker_request("GET /containers/json HTTP/1.0\r\nHost: docker\r\n\r\n", &resp, &len) != 0) return -1;
    char *body;
    size_t body_len;
    int status = wp_http_parse(resp, len, &body, &body_len);
    int r = status == 200 ? wp_docker_parse(body, body_len, out, count) : -1;
    if (r != 0 && getenv("WHOPORT_DEBUG")) {
        debug_dump(resp, len);
        if (status == 200) {
            size_t at = parse_error_at < 80 ? 0 : parse_error_at - 80;
            fprintf(stderr, "whoport debug: JSON parse stopped at byte %zu of the body, around:\n  ", parse_error_at);
            debug_dump_raw(body + at, body_len - at < 160 ? body_len - at : 160);
        }
    }
    if (getenv("WHOPORT_DEBUG"))
        fprintf(stderr, "whoport debug: docker status %d, parse %s, %zu published ports\n", status, r == 0 ? "ok" : "failed",
                *count);
    free(resp);
    if (r == 0) fill_started(*out, *count);
    return r;
}

/* Stops a container by name. Returns 0 when it stopped or was not running. */
/* Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm), so we
 * need neither timegm nor _mkgmtime. */
static long long days_from_civil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? (unsigned)-3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

long long wp_parse_rfc3339(const char *s) {
    int y, mo, d, h, mi, sec, n = 0;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) != 6 || n != 19) return -1;
    if (y < 1971 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) return -1;
    const char *p = s + 19;
    if (*p == '.')
        for (p++; *p >= '0' && *p <= '9'; p++) {}
    long long offset = 0;
    if (*p == '+' || *p == '-') {
        int oh, om;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) != 2) return -1;
        offset = (*p == '+' ? 1 : -1) * (oh * 3600LL + om * 60LL);
    } else if (*p != 'Z') {
        return -1;
    }
    return days_from_civil(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600LL + mi * 60LL + sec - offset;
}

long long wp_docker_started_at(const char *json) {
    const char *k = strstr(json, "\"StartedAt\"");
    if (!k) return -1;
    k += 11;
    while (*k == ' ' || *k == ':') k++;
    if (*k != '"') return -1;
    return wp_parse_rfc3339(k + 1); /* "0001-01-01T00:00:00Z" (never started) is rejected */
}

static int safe_name(const char *name) {
    if (!name[0]) return 0;
    for (const char *c = name; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("_.-", *c)))
            return 0;
    return 1;
}

/* The list only has the creation time; a container restarted after a reboot
 * would look days old. Ask each container for its real start time. */
static void fill_started(wp_container *list, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && !strcmp(list[i].name, list[i - 1].name)) {
            list[i].started = list[i - 1].started;
            continue;
        }
        if (!safe_name(list[i].name)) continue;
        char request[256], *resp, *body;
        size_t len, body_len;
        snprintf(request, sizeof request, "GET /containers/%s/json HTTP/1.0\r\nHost: docker\r\n\r\n", list[i].name);
        if (docker_request(request, &resp, &len) != 0) continue;
        if (wp_http_parse(resp, len, &body, &body_len) == 200) {
            body[body_len] = '\0';
            long long t = wp_docker_started_at(body);
            if (t > 0) list[i].started = t;
        }
        free(resp);
    }
}

int wp_docker_stop(const char *name, char *err, size_t err_size) {
    char request[512];
    char *resp;
    size_t len;
    if (!safe_name(name)) {
        snprintf(err, err_size, "unexpected container name");
        return -1;
    }
    snprintf(request, sizeof request,
             "POST /containers/%s/stop?t=10 HTTP/1.0\r\nHost: docker\r\nContent-Length: 0\r\n\r\n", name);
    if (docker_request(request, &resp, &len) != 0) {
        snprintf(err, err_size, "could not reach Docker");
        return -1;
    }
    char *body;
    size_t body_len;
    int status = wp_http_parse(resp, len, &body, &body_len);
    free(resp);
    if (status == 204 || status == 304) return 0;
    snprintf(err, err_size, "Docker answered with status %d", status);
    return -1;
}
