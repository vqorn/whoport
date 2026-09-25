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
