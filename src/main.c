#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "server.h"
#include "util.h"

#define DEFAULT_PORT 6380 /* 참고용 Redis 8(6379)과 동시에 띄우기 위해 (design TD-007) */

typedef struct {
    int port;
    int verbose;
} config_t;

static void usage(const char *prog) {
    fprintf(stderr, "usage: %s [--port N] [-v]\n", prog);
}

/* 성공 0, 잘못된 인자 -1, --help 1 */
static int parse_args(int argc, char **argv, config_t *cfg) {
    cfg->port = DEFAULT_PORT;
    cfg->verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            long long port;
            const char *s = argv[++i];
            if (!util_str_to_ll(s, strlen(s), &port) || port < 1 || port > 65535) {
                fprintf(stderr, "invalid port: %s\n", s);
                return -1;
            }
            cfg->port = (int)port;
        } else if (strcmp(argv[i], "-v") == 0) {
            cfg->verbose = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            return 1;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return -1;
        }
    }
    return 0;
}

static void on_stop_signal(int sig) {
    (void)sig;
    server_request_stop(); /* 플래그만 세우고, 실제 정리는 루프가 끝난 뒤 main 에서 */
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    /* 끊긴 소켓에 write 하면 SIGPIPE 로 프로세스가 죽는다 → 무시하고 EPIPE 로 처리 */
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);

    /* SA_RESTART 없이 등록해야 epoll_wait 가 EINTR 로 깨어난다 */
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int main(int argc, char **argv) {
    config_t cfg;
    int rc = parse_args(argc, argv, &cfg);
    if (rc != 0) {
        usage(argv[0]);
        return rc < 0 ? 1 : 0;
    }

    setvbuf(stdout, NULL, _IOLBF, 0); /* 로그를 줄 단위로 바로 출력 */
    setup_signals();

    if (server_init(cfg.port, cfg.verbose) != 0) {
        server_shutdown();
        return 1;
    }
    printf("mini-redis: listening on port %d (pid %d)\n", cfg.port, (int)getpid());

    server_run();

    printf("mini-redis: shutting down\n");
    server_shutdown();
    return 0;
}
