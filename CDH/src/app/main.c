/*
 * mini-redis-server CLI 진입점.
 *
 * 인자를 해석해 서버를 만들고(AOF 재생 포함) 이벤트 루프를 돌린다. SIGINT/SIGTERM 은
 * 플래그만 세우고(시그널 핸들러에서 안전한 유일한 일), 루프가 그 플래그를 보고 빠져나온
 * 뒤 정상 경로로 AOF fsync·정리를 한다. 그래서 Ctrl-C 로 꺼도 종료 코드는 0 이다.
 */
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniredis/command.h"
#include "miniredis/server.h"

/* 시그널 핸들러가 쓰는 유일한 전역 상태(sig_atomic_t 대입만 async-signal-safe). */
static volatile sig_atomic_t g_stop_signal = 0;

static void on_stop_signal(int sig) { g_stop_signal = sig; }

static void usage(FILE *f, const char *prog) {
    fprintf(f,
            "Usage: %s [options]\n"
            "  --port N                TCP port (default 6379, 0 = ephemeral)\n"
            "  --bind ADDR             bind address (default 127.0.0.1)\n"
            "  --aof PATH              enable append-only file persistence at PATH\n"
            "  --appendfsync POLICY    always | everysec | no (default everysec)\n"
            "  --maxclients N          max concurrent clients (default 10000)\n"
            "  --timeout SEC           close clients idle for SEC seconds (default 0 = off)\n"
            "  --log-level LEVEL       debug | info | warn | error (default info)\n"
            "  --help                  show this help\n"
            "  --version               show version\n",
            prog);
}

/* 10진 정수 하나를 [lo, hi] 범위로 엄격히 해석한다. */
static bool parse_long(const char *s, long lo, long hi, long *out) {
    if (s == NULL || *s == '\0') {
        return false;
    }
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

static bool parse_fsync(const char *s, mr_aof_fsync *out) {
    if (strcmp(s, "always") == 0) {
        *out = MR_AOF_FSYNC_ALWAYS;
    } else if (strcmp(s, "everysec") == 0) {
        *out = MR_AOF_FSYNC_EVERYSEC;
    } else if (strcmp(s, "no") == 0) {
        *out = MR_AOF_FSYNC_NO;
    } else {
        return false;
    }
    return true;
}

typedef enum parse_outcome { PARSE_RUN, PARSE_EXIT_OK, PARSE_EXIT_USAGE } parse_outcome;

/* 값을 받는 옵션 하나를 적용. 잘못된 값이면 false. */
static bool apply_option(mr_server_config *cfg, const char *name, const char *val) {
    long v;
    if (strcmp(name, "--port") == 0) {
        if (!parse_long(val, 0, 65535, &v)) {
            return false;
        }
        cfg->port = (uint16_t)v;
    } else if (strcmp(name, "--bind") == 0) {
        cfg->bind_addr = val;
    } else if (strcmp(name, "--aof") == 0) {
        cfg->aof_path = val;
    } else if (strcmp(name, "--appendfsync") == 0) {
        return parse_fsync(val, &cfg->appendfsync);
    } else if (strcmp(name, "--maxclients") == 0) {
        if (!parse_long(val, 1, 1000000, &v)) {
            return false;
        }
        cfg->maxclients = (size_t)v;
    } else if (strcmp(name, "--timeout") == 0) {
        if (!parse_long(val, 0, INT_MAX, &v)) {
            return false;
        }
        cfg->idle_timeout_ms = (int64_t)v * 1000;
    } else if (strcmp(name, "--log-level") == 0) {
        int lv = mr_log_level_parse(val);
        if (lv < 0) {
            return false;
        }
        cfg->log_level = (mr_log_level)lv;
    } else {
        return false;
    }
    return true;
}

static parse_outcome parse_args(int argc, char **argv, mr_server_config *cfg) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(stdout, argv[0]);
            return PARSE_EXIT_OK;
        }
        if (strcmp(a, "--version") == 0 || strcmp(a, "-v") == 0) {
            printf("mini-redis-server %s\n", MR_SERVER_VERSION);
            return PARSE_EXIT_OK;
        }
        /* "--opt=value" 와 "--opt value" 둘 다 받는다. */
        char name[32];
        const char *val;
        const char *eq = strchr(a, '=');
        if (eq != NULL && (size_t)(eq - a) < sizeof(name)) {
            memcpy(name, a, (size_t)(eq - a));
            name[eq - a] = '\0';
            val = eq + 1;
        } else if (strlen(a) < sizeof(name) && i + 1 < argc) {
            strcpy(name, a);
            val = argv[++i];
        } else {
            fprintf(stderr, "%s: invalid or incomplete option '%s'\n", argv[0], a);
            return PARSE_EXIT_USAGE;
        }
        if (!apply_option(cfg, name, val)) {
            fprintf(stderr, "%s: invalid option or value: %s %s\n", argv[0], name, val);
            return PARSE_EXIT_USAGE;
        }
    }
    return PARSE_RUN;
}

static bool install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    /* SA_RESTART 를 쓰지 않는다: poll 이 EINTR 로 즉시 깨어나 종료가 빨라진다. */
    sa.sa_handler = on_stop_signal;
    if (sigaction(SIGINT, &sa, NULL) != 0 || sigaction(SIGTERM, &sa, NULL) != 0) {
        return false;
    }
    /* 끊긴 소켓에 쓰면 프로세스가 죽지 않고 EPIPE 를 받게 한다(send 는 MSG_NOSIGNAL 도 씀). */
    sa.sa_handler = SIG_IGN;
    return sigaction(SIGPIPE, &sa, NULL) == 0;
}

int main(int argc, char **argv) {
    mr_server_config cfg;
    mr_server_config_default(&cfg);
    parse_outcome po = parse_args(argc, argv, &cfg);
    if (po == PARSE_EXIT_OK) {
        return 0;
    }
    if (po == PARSE_EXIT_USAGE) {
        usage(stderr, argv[0]);
        return 2;
    }
    if (!install_signals()) {
        mr_log(cfg.log_level, MR_LOG_ERROR, "failed to install signal handlers");
        return 1;
    }
    cfg.stop_flag = &g_stop_signal;

    char err[256] = "";
    mr_server *srv = NULL;
    mr_status st = mr_server_create(&cfg, &srv, err, sizeof(err));
    if (st != MR_OK) {
        mr_log(cfg.log_level, MR_LOG_ERROR, "startup failed: %s", err);
        return 1;
    }
    mr_log(cfg.log_level, MR_LOG_INFO,
           "mini-redis %s ready: single-threaded, listening on %s:%u (aof=%s)", MR_SERVER_VERSION,
           cfg.bind_addr, (unsigned)mr_server_port(srv),
           cfg.aof_path != NULL ? cfg.aof_path : "off");

    st = mr_server_run(srv);
    if (st != MR_OK) {
        mr_log(cfg.log_level, MR_LOG_ERROR, "event loop failed: %s", mr_status_str(st));
    } else {
        mr_log(cfg.log_level, MR_LOG_INFO, "received signal %d, shutting down",
               (int)g_stop_signal);
    }
    mr_status cst = mr_server_destroy(srv);
    if (st == MR_OK && cst == MR_OK) {
        mr_log(cfg.log_level, MR_LOG_INFO, "bye");
        return 0;
    }
    return 1;
}
