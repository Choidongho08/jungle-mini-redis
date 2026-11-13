#include "miniredis/server.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "miniredis/command.h"
#include "miniredis/store.h"

/*
 * 이벤트 루프 구현.
 *
 * 한 틱(mr_server_run_once)의 순서
 *   1) pollfd 배열 구성: 리스닝 소켓 + 연결마다 POLLIN(더 읽을 수 있을 때),
 *      POLLOUT(보낼 출력이 남았을 때만 — 항상 걸면 쓰기 가능 이벤트가 매번 와서
 *      루프가 바쁜 대기에 빠진다).
 *   2) poll. 처리 못 한 입력이 남은 연결이 있으면 timeout 0 으로 즉시 돌아온다.
 *   3) 새 연결 수락(틱당 상한) → 연결별 읽기 → 명령 실행 → 즉시 쓰기 시도.
 *      응답을 곧바로 write 해 보는 이유: 대부분은 소켓 버퍼에 다 들어가므로
 *      POLLOUT 을 기다리는 왕복 한 번을 아낀다. 다 못 쓴 부분만 POLLOUT 으로 넘긴다.
 *   4) cron 주기가 지났으면 sweep(시간 예산)·AOF tick·idle timeout 검사.
 *   5) 닫힌 연결 회수(배열 압축). 틱 도중에는 인덱스가 pollfd 와 대응하므로
 *      지우지 않고 표시만 한다.
 */

#define DEFAULT_MAXCLIENTS 10000u
#define DEFAULT_MAX_QUERY ((size_t)128 * 1024 * 1024)
#define DEFAULT_MAX_OUTPUT ((size_t)256 * 1024 * 1024)
#define DEFAULT_CMDS_PER_TICK 1000u
#define DEFAULT_READ_PER_TICK ((size_t)1024 * 1024)
#define DEFAULT_CRON_MS 100
#define DEFAULT_SWEEP_SAMPLES 1000u
#define DEFAULT_SWEEP_BUDGET_MS 5

#define READ_CHUNK ((size_t)16 * 1024)
/* 이보다 큰 버퍼는 비었을 때 반납한다(큰 값 한 번 뒤 메모리를 계속 쥐지 않게). */
#define BUF_SHRINK_BYTES ((size_t)1024 * 1024)
/* 출력 앞부분이 이만큼 전송되면 남은 부분을 당겨 버퍼가 무한히 자라지 않게 한다. */
#define OUT_COMPACT_BYTES ((size_t)64 * 1024)
#define ACCEPT_PER_TICK 128
#define LISTEN_BACKLOG 511
/* maxclients 외에 리스닝 소켓·AOF·로그 등이 쓸 fd 여유 */
#define RESERVED_FDS 32

#define LOG(s, lvl, ...) mr_log((s)->cfg.log_level, (lvl), __VA_ARGS__)

static const char MAXCLIENTS_ERR[] = "-ERR max number of clients reached\r\n";

typedef struct conn {
    int fd;
    uint64_t id;
    mr_buf in;       /* in.data[in_pos..in.len) 이 아직 처리되지 않은 입력 */
    size_t in_pos;
    mr_buf out;      /* out.data[out_sent..out.len) 이 아직 보내지 않은 출력 */
    size_t out_sent;
    mr_resp_parser parser;
    int64_t last_active_ms;
    bool close_after_write; /* QUIT/프로토콜 오류/EOF: 출력을 비운 뒤 닫는다 */
    bool input_pending;     /* 틱 예산 때문에 처리하지 못한 완성 명령이 남았을 수 있음 */
    bool closed;            /* fd 를 닫았음: 틱 끝에 회수 */
    char addr[64];
} conn;

struct mr_server {
    mr_server_config cfg;
    char *bind_addr;
    char *aof_path;
    const mr_clock *clock;
    int listen_fd;
    uint16_t port;
    mr_store *store;
    mr_aof *aof;
    mr_cmd_env env;
    mr_cmd_server_info info;
    conn **conns;
    size_t nconns;
    size_t conns_cap;
    struct pollfd *pfds;
    size_t pfds_cap;
    uint64_t next_conn_id;
    int64_t next_cron_ms;
    volatile sig_atomic_t stop;
};

/* ---------- 설정 ---------- */

void mr_server_config_default(mr_server_config *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->bind_addr = "127.0.0.1";
    cfg->port = 6379;
    cfg->aof_path = NULL;
    cfg->appendfsync = MR_AOF_FSYNC_EVERYSEC;
    cfg->maxclients = DEFAULT_MAXCLIENTS;
    cfg->idle_timeout_ms = 0;
    cfg->max_query_bytes = DEFAULT_MAX_QUERY;
    cfg->max_output_bytes = DEFAULT_MAX_OUTPUT;
    cfg->max_cmds_per_tick = DEFAULT_CMDS_PER_TICK;
    cfg->max_read_per_tick = DEFAULT_READ_PER_TICK;
    cfg->cron_interval_ms = DEFAULT_CRON_MS;
    cfg->sweep_samples = DEFAULT_SWEEP_SAMPLES;
    cfg->sweep_budget_ms = DEFAULT_SWEEP_BUDGET_MS;
    cfg->clock = NULL;
    cfg->log_level = MR_LOG_INFO;
    cfg->stop_flag = NULL;
}

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (errbuf == NULL || errlen == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

static const char *fsync_name(mr_aof_fsync f) {
    switch (f) {
    case MR_AOF_FSYNC_ALWAYS:
        return "always";
    case MR_AOF_FSYNC_EVERYSEC:
        return "everysec";
    case MR_AOF_FSYNC_NO:
        return "no";
    }
    return "?";
}

static bool config_valid(const mr_server_config *c, char *errbuf, size_t errlen) {
    if (c->maxclients == 0 || c->max_query_bytes == 0 || c->max_output_bytes == 0 ||
        c->max_cmds_per_tick == 0 || c->max_read_per_tick == 0 || c->cron_interval_ms <= 0 ||
        c->cron_interval_ms > 60000 || c->idle_timeout_ms < 0 || c->sweep_budget_ms < 0) {
        set_err(errbuf, errlen, "invalid server configuration (zero/negative limit)");
        return false;
    }
    if (c->appendfsync != MR_AOF_FSYNC_ALWAYS && c->appendfsync != MR_AOF_FSYNC_EVERYSEC &&
        c->appendfsync != MR_AOF_FSYNC_NO) {
        set_err(errbuf, errlen, "invalid appendfsync policy");
        return false;
    }
    return true;
}

static char *dup_or_null(const char *s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d != NULL) {
        memcpy(d, s, n);
    }
    return d;
}

/* ---------- 소켓 도우미 ---------- */

static bool set_nonblock_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) != 0) {
        return false;
    }
    int fdfl = fcntl(fd, F_GETFD, 0);
    return fdfl >= 0 && fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) == 0;
}

static uint16_t local_port(int fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        return ntohs(((struct sockaddr_in *)&ss)->sin_port);
    }
    if (ss.ss_family == AF_INET6) {
        return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    }
    return 0;
}

/* 주소 하나에 대해 socket → SO_REUSEADDR → bind → listen. 실패 시 errno 보존. */
static int listen_on(const struct addrinfo *ai) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
        return -1;
    }
    /* TIME_WAIT 에 남은 이전 연결 때문에 재시작 직후 bind 가 실패하지 않게 한다. */
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        bind(fd, ai->ai_addr, ai->ai_addrlen) != 0 || listen(fd, LISTEN_BACKLOG) != 0 ||
        !set_nonblock_cloexec(fd)) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static mr_status open_listener(mr_server *s, char *errbuf, size_t errlen) {
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)s->cfg.port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(s->bind_addr, port, &hints, &res);
    if (rc != 0) {
        set_err(errbuf, errlen, "resolve bind address '%s': %s", s->bind_addr, gai_strerror(rc));
        return MR_INVALID_ARG;
    }
    int err = 0;
    for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next) {
        s->listen_fd = listen_on(ai);
        if (s->listen_fd >= 0) {
            break;
        }
        err = errno;
    }
    freeaddrinfo(res);
    if (s->listen_fd < 0) {
        set_err(errbuf, errlen, "bind %s:%s: %s", s->bind_addr, port, strerror(err));
        return MR_IO_ERROR;
    }
    s->port = local_port(s->listen_fd);
    return MR_OK;
}

/* ---------- 저장소/AOF ---------- */

/* 해시 플러딩 방지용 시드. /dev/urandom 이 없으면 시각·pid 로 대신한다(약하지만 동작). */
static void random_seed(uint8_t seed[16]) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, seed, 16);
        close(fd);
        if (n == 16) {
            return;
        }
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t a = (uint64_t)ts.tv_nsec ^ ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)getpid();
    uint64_t b = a * 0x9E3779B97F4A7C15ull;
    memcpy(seed, &a, 8);
    memcpy(seed + 8, &b, 8);
}

static mr_status open_storage(mr_server *s, char *errbuf, size_t errlen) {
    mr_store_opts so;
    memset(&so, 0, sizeof(so));
    so.clock = s->clock;
    random_seed(so.hash_seed);
    mr_status st = mr_store_create(&so, &s->store);
    if (st != MR_OK) {
        set_err(errbuf, errlen, "create store: %s", mr_status_str(st));
        return st;
    }
    if (s->aof_path == NULL) {
        return MR_OK;
    }
    mr_aof_opts ao = {s->aof_path, s->cfg.appendfsync, s->clock};
    mr_aof_load_info li;
    memset(&li, 0, sizeof(li));
    int64_t t0 = mr_clock_mono_ms(s->clock);
    st = mr_aof_open(&ao, s->store, &s->aof, &li);
    if (st != MR_OK) {
        set_err(errbuf, errlen, "open AOF '%s': %s", s->aof_path, mr_status_str(st));
        return st;
    }
    LOG(s, MR_LOG_INFO,
        "AOF loaded from %s: %" PRIu64 " records, %zu keys, %zu expired dropped, %" PRIu64
        " truncated bytes, %" PRId64 " ms",
        s->aof_path, li.records, mr_store_size(s->store), li.expired_dropped, li.truncated_bytes,
        mr_clock_mono_ms(s->clock) - t0);
    if (li.truncated_bytes > 0) {
        LOG(s, MR_LOG_WARN, "AOF had an incomplete tail record; truncated %" PRIu64 " bytes",
            li.truncated_bytes);
    }
    return MR_OK;
}

/*
 * maxclients 만큼 fd 를 쓸 수 있는지 확인하고, 모자라면 한도를 올려 보고, 그래도
 * 모자라면 maxclients 를 줄인다(Redis 와 같은 정책). accept 가 EMFILE 로 계속 실패하는
 * 것보다 시작 시점에 명시적으로 알리는 편이 낫다.
 */
static void fit_maxclients_to_fd_limit(mr_server *s) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        return;
    }
    rlim_t need = (rlim_t)s->cfg.maxclients + RESERVED_FDS;
    if (rl.rlim_cur >= need) {
        return;
    }
    struct rlimit want = rl;
    want.rlim_cur = (rl.rlim_max == RLIM_INFINITY || rl.rlim_max >= need) ? need : rl.rlim_max;
    if (setrlimit(RLIMIT_NOFILE, &want) == 0) {
        rl = want;
    }
    if (rl.rlim_cur < need && rl.rlim_cur > RESERVED_FDS) {
        size_t fit = (size_t)(rl.rlim_cur - RESERVED_FDS);
        LOG(s, MR_LOG_WARN, "fd limit %llu too low; maxclients reduced from %zu to %zu",
            (unsigned long long)rl.rlim_cur, s->cfg.maxclients, fit);
        s->cfg.maxclients = fit;
    }
}

/* ---------- 생성/해제 ---------- */

static void init_env(mr_server *s) {
    s->info.start_mono_ms = mr_clock_mono_ms(s->clock);
    s->info.port = s->port;
    s->info.bind_addr = s->bind_addr;
    s->info.maxclients = s->cfg.maxclients;
    s->info.idle_timeout_sec = s->cfg.idle_timeout_ms / 1000;
    s->info.appendfsync = fsync_name(s->cfg.appendfsync);
    s->env.store = s->store;
    s->env.aof = s->aof;
    s->env.clock = s->clock;
    s->env.info = &s->info;
    s->env.aof_write_failed = false;
    s->env.aof_rewrites = 0;
    s->env.aof_last_rewrite_ok = true;
    s->env.aof_last_rewrite_ms = -1;
}

mr_status mr_server_create(const mr_server_config *cfg, mr_server **out, char *errbuf,
                           size_t errlen) {
    if (cfg == NULL || out == NULL) {
        set_err(errbuf, errlen, "NULL argument");
        return MR_INVALID_ARG;
    }
    *out = NULL;
    if (!config_valid(cfg, errbuf, errlen)) {
        return MR_INVALID_ARG;
    }
    mr_server *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        set_err(errbuf, errlen, "out of memory");
        return MR_NOMEM;
    }
    s->cfg = *cfg;
    s->listen_fd = -1;
    s->clock = cfg->clock != NULL ? cfg->clock : mr_clock_system();
    s->bind_addr = dup_or_null(cfg->bind_addr != NULL ? cfg->bind_addr : "127.0.0.1");
    s->aof_path = dup_or_null(cfg->aof_path);
    if (s->bind_addr == NULL || (cfg->aof_path != NULL && s->aof_path == NULL)) {
        set_err(errbuf, errlen, "out of memory");
        mr_server_destroy(s);
        return MR_NOMEM;
    }
    /* 저장된 문자열이 호출자 수명에 묶이지 않도록 사본을 가리키게 한다. */
    s->cfg.bind_addr = s->bind_addr;
    s->cfg.aof_path = s->aof_path;
    fit_maxclients_to_fd_limit(s);
    /* AOF 를 먼저 재생한다: 포트를 연 뒤 재생하면 그동안 접속한 클라이언트가 빈 DB 를 본다. */
    mr_status st = open_storage(s, errbuf, errlen);
    if (st == MR_OK) {
        st = open_listener(s, errbuf, errlen);
    }
    if (st != MR_OK) {
        mr_server_destroy(s);
        return st;
    }
    init_env(s);
    s->next_cron_ms = mr_clock_mono_ms(s->clock) + s->cfg.cron_interval_ms;
    *out = s;
    return MR_OK;
}

/* ---------- 연결 ---------- */

static size_t pending_out(const conn *c) { return c->out.len - c->out_sent; }

static void conn_close(mr_server *s, conn *c, const char *reason) {
    if (c->closed) {
        return;
    }
    LOG(s, MR_LOG_DEBUG, "client #%" PRIu64 " %s closed: %s", c->id, c->addr, reason);
    close(c->fd);
    c->fd = -1;
    c->closed = true;
    mr_buf_free(&c->in);
    mr_buf_free(&c->out);
    mr_resp_parser_free(&c->parser);
    s->info.connected_clients--;
}

static void peer_name(int fd, char *out, size_t cap) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof(ss);
    char host[48];
    char serv[8];
    if (getpeername(fd, (struct sockaddr *)&ss, &len) != 0 ||
        getnameinfo((struct sockaddr *)&ss, len, host, sizeof(host), serv, sizeof(serv),
                    NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        snprintf(out, cap, "?");
        return;
    }
    snprintf(out, cap, "%s:%s", host, serv);
}

static bool conns_grow(mr_server *s) {
    if (s->nconns < s->conns_cap) {
        return true;
    }
    size_t ncap = s->conns_cap != 0 ? s->conns_cap * 2 : 16;
    conn **nc = realloc(s->conns, ncap * sizeof(*nc));
    if (nc == NULL) {
        return false;
    }
    s->conns = nc;
    s->conns_cap = ncap;
    return true;
}

static bool conn_add(mr_server *s, int fd) {
    conn *c = calloc(1, sizeof(*c));
    if (c == NULL || !conns_grow(s)) {
        free(c);
        return false;
    }
    c->fd = fd;
    c->id = ++s->next_conn_id;
    mr_resp_parser_init(&c->parser, &s->cfg.proto);
    c->last_active_ms = mr_clock_mono_ms(s->clock);
    peer_name(fd, c->addr, sizeof(c->addr));
    s->conns[s->nconns++] = c;
    s->info.total_connections++;
    s->info.connected_clients++;
    LOG(s, MR_LOG_DEBUG, "client #%" PRIu64 " %s connected", c->id, c->addr);
    return true;
}

/* maxclients 초과: 오류 한 줄을 (블록하지 않고) 시도해 보고 바로 닫는다. */
static void reject_client(mr_server *s, int fd) {
    ssize_t n = send(fd, MAXCLIENTS_ERR, sizeof(MAXCLIENTS_ERR) - 1, MSG_NOSIGNAL);
    (void)n;
    close(fd);
    s->info.rejected_connections++;
    LOG(s, MR_LOG_WARN, "rejected connection: max number of clients (%zu) reached",
        s->cfg.maxclients);
}

static void accept_clients(mr_server *s) {
    for (int i = 0; i < ACCEPT_PER_TICK; i++) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) {
                continue;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG(s, MR_LOG_WARN, "accept: %s", strerror(errno));
            }
            return;
        }
        if (!set_nonblock_cloexec(fd)) {
            close(fd);
            continue;
        }
        /* 요청-응답이 작은 패킷 위주라 Nagle 지연(최대 수십 ms)을 끈다. 실패해도 동작한다. */
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (s->info.connected_clients >= s->cfg.maxclients) {
            reject_client(s, fd);
            continue;
        }
        if (!conn_add(s, fd)) {
            LOG(s, MR_LOG_ERROR, "out of memory accepting client");
            close(fd);
        }
    }
}

/* 출력 버퍼를 가능한 만큼 보낸다. EAGAIN 이면 남겨 두고 POLLOUT 을 기다린다. */
static void conn_flush(mr_server *s, conn *c) {
    while (!c->closed && c->out_sent < c->out.len) {
        ssize_t n = send(c->fd, c->out.data + c->out_sent, c->out.len - c->out_sent,
                         MSG_NOSIGNAL);
        if (n > 0) {
            c->out_sent += (size_t)n;
            s->info.total_net_output_bytes += (uint64_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        conn_close(s, c, n < 0 ? strerror(errno) : "send returned 0");
        return;
    }
    if (c->closed) {
        return;
    }
    if (c->out_sent == c->out.len) {
        c->out.len = 0;
        c->out_sent = 0;
        if (c->out.cap > BUF_SHRINK_BYTES) {
            mr_buf_free(&c->out);
        }
        /* EOF 뒤에도 예산 때문에 남은 명령이 있으면 다 처리한 다음에 닫는다. */
        if (c->close_after_write && !c->input_pending) {
            conn_close(s, c, "closed after reply");
        }
    } else if (c->out_sent >= OUT_COMPACT_BYTES && c->out_sent > c->out.len / 2) {
        memmove(c->out.data, c->out.data + c->out_sent, c->out.len - c->out_sent);
        c->out.len -= c->out_sent;
        c->out_sent = 0;
    }
}

/* 틱 예산 안에서 읽는다. EOF 면 남은 명령을 처리한 뒤 닫도록 표시한다. */
static void conn_read(mr_server *s, conn *c) {
    size_t got = 0;
    while (got < s->cfg.max_read_per_tick) {
        if (mr_buf_reserve(&c->in, READ_CHUNK) != MR_OK) {
            conn_close(s, c, "out of memory for input buffer");
            return;
        }
        size_t room = c->in.cap - c->in.len;
        size_t left = s->cfg.max_read_per_tick - got;
        ssize_t n = recv(c->fd, c->in.data + c->in.len, room < left ? room : left, 0);
        if (n > 0) {
            c->in.len += (size_t)n;
            got += (size_t)n;
            s->info.total_net_input_bytes += (uint64_t)n;
            c->last_active_ms = mr_clock_mono_ms(s->clock);
            continue;
        }
        if (n == 0) {
            c->close_after_write = true;
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            conn_close(s, c, strerror(errno));
            return;
        }
        break;
    }
    if (c->in.len - c->in_pos > s->cfg.max_query_bytes) {
        LOG(s, MR_LOG_WARN, "client #%" PRIu64 " %s exceeded query buffer limit (%zu bytes)",
            c->id, c->addr, s->cfg.max_query_bytes);
        conn_close(s, c, "query buffer limit");
    }
}

/* 처리한 입력을 버퍼 앞에서 걷어낸다. 파서는 오프셋 기반이라 당겨도 안전하다. */
static void compact_input(conn *c) {
    if (c->in_pos == c->in.len) {
        c->in.len = 0;
        c->in_pos = 0;
        if (c->in.cap > BUF_SHRINK_BYTES) {
            mr_buf_free(&c->in);
        }
    } else if (c->in_pos > 0) {
        memmove(c->in.data, c->in.data + c->in_pos, c->in.len - c->in_pos);
        c->in.len -= c->in_pos;
        c->in_pos = 0;
    }
}

static void protocol_error(mr_server *s, conn *c) {
    const char *msg = mr_resp_error_msg(&c->parser);
    LOG(s, MR_LOG_INFO, "client #%" PRIu64 " %s: %s", c->id, c->addr, msg);
    /* 스트림 경계를 잃었으므로 남은 입력은 버리고 오류만 보낸 뒤 닫는다. */
    c->in_pos = c->in.len;
    c->close_after_write = true;
    if (mr_resp_errorf(&c->out, "ERR %s", msg) != MR_OK) {
        conn_close(s, c, "out of memory");
    }
}

/* 출력 상한 검사. 먼저 보내 보고도 넘치면 느린 클라이언트로 보고 끊는다. */
static bool output_within_limit(mr_server *s, conn *c) {
    if (pending_out(c) <= s->cfg.max_output_bytes) {
        return true;
    }
    conn_flush(s, c);
    if (c->closed || pending_out(c) <= s->cfg.max_output_bytes) {
        return !c->closed;
    }
    LOG(s, MR_LOG_WARN, "client #%" PRIu64 " %s exceeded output buffer limit (%zu bytes)", c->id,
        c->addr, s->cfg.max_output_bytes);
    conn_close(s, c, "output buffer limit");
    return false;
}

/* 버퍼에 완성된 명령을 틱 예산만큼 실행한다. */
static void conn_process(mr_server *s, conn *c) {
    size_t done = 0;
    c->input_pending = false;
    while (!c->closed && c->in_pos < c->in.len) {
        if (done >= s->cfg.max_cmds_per_tick) {
            c->input_pending = true; /* 다른 연결에 차례를 넘기고 다음 틱에 이어서 */
            break;
        }
        size_t used = 0;
        mr_resp_result r = mr_resp_parse(&c->parser, c->in.data + c->in_pos,
                                         c->in.len - c->in_pos, &used);
        if (r == MR_RESP_NEED_MORE) {
            break;
        }
        if (r == MR_RESP_ERROR) {
            protocol_error(s, c);
            break;
        }
        if (r == MR_RESP_NOMEM) {
            conn_close(s, c, "out of memory parsing request");
            return;
        }
        unsigned flags = 0;
        mr_status st = mr_cmd_exec(&s->env, mr_resp_argc(&c->parser), mr_resp_argv(&c->parser),
                                   &c->out, &flags);
        c->in_pos += used;
        done++;
        if (st != MR_OK) {
            conn_close(s, c, "out of memory building reply");
            return;
        }
        if (!output_within_limit(s, c)) {
            return;
        }
        if ((flags & MR_CMD_RESULT_CLOSE) != 0) {
            /* QUIT 뒤에 파이프라인된 명령은 실행하지 않는다. */
            c->close_after_write = true;
            c->in_pos = c->in.len;
            break;
        }
    }
    if (!c->closed) {
        compact_input(c);
    }
}

/* ---------- cron ---------- */

static void close_idle(mr_server *s, int64_t now) {
    if (s->cfg.idle_timeout_ms <= 0) {
        return;
    }
    for (size_t i = 0; i < s->nconns; i++) {
        conn *c = s->conns[i];
        if (!c->closed && now - c->last_active_ms > s->cfg.idle_timeout_ms) {
            conn_close(s, c, "idle timeout");
        }
    }
}

static void cron(mr_server *s, int64_t now) {
    (void)mr_store_sweep(s->store, s->cfg.sweep_samples, s->cfg.sweep_budget_ms);
    if (s->aof != NULL) {
        mr_status st = mr_aof_tick(s->aof);
        if (st != MR_OK && !s->env.aof_write_failed) {
            /* fsync 실패 후에는 무엇이 디스크에 있는지 알 수 없다: 쓰기를 막는다. */
            LOG(s, MR_LOG_ERROR, "AOF fsync failed: %s; write commands disabled",
                mr_status_str(st));
            s->env.aof_write_failed = true;
        }
    }
    close_idle(s, now);
}

/* 닫힌 연결을 배열에서 빼고 해제한다(순서 유지: 공정성에 영향 없음). */
static void reap(mr_server *s) {
    size_t w = 0;
    for (size_t i = 0; i < s->nconns; i++) {
        if (s->conns[i]->closed) {
            free(s->conns[i]);
        } else {
            s->conns[w++] = s->conns[i];
        }
    }
    s->nconns = w;
}

/* ---------- 루프 ---------- */

static bool build_pollfds(mr_server *s, bool *busy) {
    size_t need = s->nconns + 1;
    if (need > s->pfds_cap) {
        struct pollfd *np = realloc(s->pfds, need * sizeof(*np));
        if (np == NULL) {
            return false;
        }
        s->pfds = np;
        s->pfds_cap = need;
    }
    s->pfds[0].fd = s->listen_fd;
    s->pfds[0].events = POLLIN;
    s->pfds[0].revents = 0;
    *busy = false;
    for (size_t i = 0; i < s->nconns; i++) {
        const conn *c = s->conns[i];
        short ev = 0;
        if (!c->close_after_write) {
            ev |= POLLIN;
        }
        if (pending_out(c) > 0) {
            ev |= POLLOUT;
        }
        s->pfds[i + 1].fd = c->fd;
        s->pfds[i + 1].events = ev;
        s->pfds[i + 1].revents = 0;
        *busy = *busy || c->input_pending;
    }
    return true;
}

static int poll_timeout(const mr_server *s, int timeout_ms, bool busy) {
    if (busy) {
        return 0;
    }
    int64_t until = s->next_cron_ms - mr_clock_mono_ms(s->clock);
    if (until < 0) {
        until = 0;
    }
    if (timeout_ms < 0 || until < timeout_ms) {
        return (int)until;
    }
    return timeout_ms;
}

static void service_conn(mr_server *s, conn *c, short re) {
    if ((re & POLLNVAL) != 0) {
        conn_close(s, c, "invalid fd");
        return;
    }
    /* POLLHUP/POLLERR 도 읽기로 처리한다: 반쯤 닫힌 연결의 남은 요청을 먼저 소비하고,
     * 오류는 recv 가 errno 로 알려 준다. */
    if (!c->close_after_write && (re & (POLLIN | POLLHUP | POLLERR)) != 0) {
        conn_read(s, c);
    }
    if (!c->closed) {
        conn_process(s, c);
    }
    if (!c->closed && (pending_out(c) > 0 || c->close_after_write)) {
        conn_flush(s, c);
    }
}

mr_status mr_server_run_once(mr_server *s, int timeout_ms) {
    bool busy;
    if (!build_pollfds(s, &busy)) {
        return MR_NOMEM;
    }
    size_t polled = s->nconns;
    int n = poll(s->pfds, (nfds_t)(polled + 1), poll_timeout(s, timeout_ms, busy));
    if (n < 0 && errno != EINTR) {
        LOG(s, MR_LOG_ERROR, "poll: %s", strerror(errno));
        return MR_IO_ERROR;
    }
    if (n > 0 && (s->pfds[0].revents & POLLIN) != 0) {
        accept_clients(s); /* 새 연결은 배열 끝에 붙으므로 polled 범위 밖: 다음 틱부터 */
    }
    for (size_t i = 0; i < polled; i++) {
        conn *c = s->conns[i];
        short re = n > 0 ? s->pfds[i + 1].revents : 0;
        if (!c->closed && (re != 0 || c->input_pending)) {
            service_conn(s, c, re);
        }
    }
    int64_t now = mr_clock_mono_ms(s->clock);
    if (now >= s->next_cron_ms) {
        cron(s, now);
        s->next_cron_ms = now + s->cfg.cron_interval_ms;
    }
    reap(s);
    return MR_OK;
}

static bool stop_requested(const mr_server *s) {
    return s->stop != 0 || (s->cfg.stop_flag != NULL && *s->cfg.stop_flag != 0);
}

mr_status mr_server_run(mr_server *s) {
    while (!stop_requested(s)) {
        mr_status st = mr_server_run_once(s, (int)s->cfg.cron_interval_ms);
        if (st != MR_OK) {
            return st;
        }
    }
    return MR_OK;
}

void mr_server_request_stop(mr_server *s) { s->stop = 1; }

uint16_t mr_server_port(const mr_server *s) { return s->port; }

size_t mr_server_client_count(const mr_server *s) { return (size_t)s->info.connected_clients; }

mr_status mr_server_destroy(mr_server *s) {
    if (s == NULL) {
        return MR_OK;
    }
    for (size_t i = 0; i < s->nconns; i++) {
        conn *c = s->conns[i];
        if (!c->closed) {
            conn_flush(s, c); /* 블록하지 않는 마지막 전송 시도 */
            conn_close(s, c, "server shutdown");
        }
        free(c);
    }
    free(s->conns);
    free(s->pfds);
    if (s->listen_fd >= 0) {
        close(s->listen_fd);
    }
    mr_status st = mr_aof_close(s->aof);
    if (st != MR_OK) {
        LOG(s, MR_LOG_ERROR, "AOF close failed: %s (recent writes may be lost)",
            mr_status_str(st));
    }
    mr_store_destroy(s->store);
    free(s->bind_addr);
    free(s->aof_path);
    free(s);
    return st;
}
