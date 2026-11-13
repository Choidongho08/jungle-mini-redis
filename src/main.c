#include <stdio.h>
#include <string.h>

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

int main(int argc, char **argv) {
    config_t cfg;
    int rc = parse_args(argc, argv, &cfg);
    if (rc != 0) {
        usage(argv[0]);
        return rc < 0 ? 1 : 0;
    }

    /* 서버 루프는 T2 에서 연결한다. */
    printf("mini-redis: port=%d verbose=%d (server not implemented yet)\n", cfg.port, cfg.verbose);
    return 0;
}
