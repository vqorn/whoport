/* Process control and environment on Linux and macOS. */
#ifndef _WIN32
#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE
#include "whoport.h"

#include <errno.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>

void wp_platform_init(void) {}

int wp_stdout_is_tty(void) {
    return isatty(STDOUT_FILENO);
}

int wp_term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
    const char *c = getenv("COLUMNS");
    return c ? atoi(c) : 0;
}

int wp_term_height(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) return ws.ws_row;
    const char *c = getenv("LINES");
    return c ? atoi(c) : 0;
}

int wp_stdin_is_tty(void) {
    return isatty(STDIN_FILENO);
}

long long wp_clock_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static struct termios saved_termios;
static int raw_active = 0;

int wp_term_raw(int enable) {
    if (enable && !raw_active) {
        if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) return -1;
        struct termios t = saved_termios;
        /* No line buffering, no echo, and Ctrl+C arrives as a key (3), so we
         * can always restore the terminal before exiting. */
        t.c_lflag &= ~(tcflag_t)(ICANON | ECHO | ISIG);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) != 0) return -1;
        raw_active = 1;
    } else if (!enable && raw_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        raw_active = 0;
    }
    return 0;
}

static int read_byte(int timeout_ms) {
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    if (poll(&p, 1, timeout_ms) <= 0) return -1;
    unsigned char c;
    return read(STDIN_FILENO, &c, 1) == 1 ? c : -1;
}

int wp_read_key(int timeout_ms) {
    int c = read_byte(timeout_ms);
    if (c < 0) return WP_KEY_NONE;
    if (c != 27) return c;
    /* Escape sequences: ESC [ A (up), ESC [ B (down), ESC [ 5 ~ (page up)... */
    int c1 = read_byte(30);
    if (c1 < 0) return WP_KEY_ESC;
    if (c1 != '[' && c1 != 'O') return WP_KEY_ESC;
    int c2 = read_byte(30);
    if (c2 == 'A') return WP_KEY_UP;
    if (c2 == 'B') return WP_KEY_DOWN;
    if (c2 == '5' || c2 == '6') {
        read_byte(30); /* '~' */
        return c2 == '5' ? WP_KEY_PGUP : WP_KEY_PGDN;
    }
    return WP_KEY_NONE;
}

int wp_open_url(const char *url) {
#ifdef __APPLE__
    const char *opener = "open";
#else
    const char *opener = "xdg-open";
#endif
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int null = open("/dev/null", O_RDWR);
        if (null >= 0) {
            dup2(null, STDIN_FILENO);
            dup2(null, STDOUT_FILENO);
            dup2(null, STDERR_FILENO);
        }
        execlp(opener, opener, url, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int wp_getcwd(char *out, size_t size) {
    return getcwd(out, size) ? 0 : -1;
}

const char *wp_home(void) {
    const char *home = getenv("HOME");
    if (home && *home) return home;
    struct passwd *pw = getpwuid(getuid());
    return pw ? pw->pw_dir : NULL;
}

int wp_is_alive(int pid) {
    return kill(pid, 0) == 0 || errno == EPERM;
}

int wp_terminate(int pid, int force, char *err, size_t err_size) {
    if (kill(pid, force ? SIGKILL : SIGTERM) == 0) return 0;
    snprintf(err, err_size, "%s", strerror(errno));
    return -1;
}

void wp_sleep_ms(int ms) {
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&t, NULL);
}

int wp_port_bindable(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    int ok = bind(fd, (struct sockaddr *)&addr, sizeof addr) == 0;
    close(fd);
    return ok;
}

void wp_localtime(long long t, struct tm *out) {
    time_t tt = (time_t)t;
    localtime_r(&tt, out);
}
#else
typedef int wp_posix_platform_unused;
#endif
