#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "miniredis/server.h"

static const char *level_name(mr_log_level level) {
    switch (level) {
    case MR_LOG_DEBUG:
        return "DEBUG";
    case MR_LOG_INFO:
        return "INFO";
    case MR_LOG_WARN:
        return "WARN";
    case MR_LOG_ERROR:
        return "ERROR";
    case MR_LOG_SILENT:
        break;
    }
    return "?";
}

void mr_log(mr_log_level threshold, mr_log_level level, const char *fmt, ...) {
    if (level < threshold || level >= MR_LOG_SILENT) {
        return;
    }
    struct timespec ts;
    struct tm tm;
    char when[32] = "?";
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && localtime_r(&ts.tv_sec, &tm) != NULL) {
        size_t n = strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm);
        snprintf(when + n, sizeof(when) - n, ".%03ld", ts.tv_nsec / 1000000L);
    }
    /* 한 줄을 버퍼에 완성한 뒤 한 번에 써서 다른 출력과 섞이지 않게 한다. */
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    fprintf(stderr, "%s [%s] %ld %s\n", when, level_name(level), (long)getpid(), msg);
    fflush(stderr);
}

int mr_log_level_parse(const char *s) {
    static const char *const names[] = {"debug", "info", "warn", "error"};
    for (int i = 0; i < 4; i++) {
        if (strcmp(s, names[i]) == 0) {
            return i;
        }
    }
    return -1;
}
