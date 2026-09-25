/* Process control and environment on Linux and macOS. */
#ifndef _WIN32
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

void wp_platform_init(void) {}

int wp_stdout_is_tty(void) {
    return isatty(STDOUT_FILENO);
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

void wp_localtime(long long t, struct tm *out) {
    time_t tt = (time_t)t;
    localtime_r(&tt, out);
}
#else
typedef int wp_posix_platform_unused;
#endif
