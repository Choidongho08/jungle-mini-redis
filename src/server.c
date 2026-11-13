/* 단일 스레드 epoll 이벤트 루프 (design §3, §8, TD-001, TD-002)
 *
 * 모든 소켓은 non-blocking 이고, epoll 이 "지금 읽을/쓸 수 있는" fd 만 알려 준다.
 * 스레드가 하나뿐이므로 명령은 항상 하나씩 끝까지 실행된다 → 락 없이 원자성 보장 (FR-003). */
#include "server.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

#include "util.h"

#define READ_CHUNK (16 * 1024)                   /* 한 번에 읽는 양 */
#define MAX_EVENTS 128
#define QUERY_BUF_LIMIT ((size_t)1024 * 1024 * 1024) /* Redis client-query-buffer-limit 기본값 1GB */

static struct {
    int listen_fd;
    int epfd;
    int verbose;
    client_t **clients; /* fd 를 인덱스로 쓰는 배열 */
    int max_fd;         /* clients 배열 크기 */
    volatile sig_atomic_t stop;
} srv = { -1, -1, 0, NULL, 0, 0 };

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ---------- 클라이언트 수명 주기 ---------- */

void client_reset_args(client_t *c);

void client_reset_args(client_t *c) {
    for (int i = 0; i < c->argc; i++) free(c->argv[i].ptr);
    c->argc = 0;
}

static client_t *client_create(int fd) {
    client_t *c = xcalloc(1, sizeof(*c));
    c->fd = fd;
    buffer_init(&c->query);
    buffer_init(&c->reply);
    c->bulk_len = -1;
    return c;
}

static void client_free(client_t *c) {
    if (srv.verbose) printf("client fd=%d closed\n", c->fd);
    epoll_ctl(srv.epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    if (c->fd < srv.max_fd) srv.clients[c->fd] = NULL;
    client_reset_args(c);
    free(c->argv);
    buffer_free(&c->query);
    buffer_free(&c->reply);
    free(c);
}

/* EPOLLOUT 감시를 켜거나 끈다. 보낼 데이터가 남았을 때만 켜야
 * level-triggered epoll 이 "쓸 수 있음" 이벤트를 계속 깨우지 않는다. */
static void client_set_write_interest(client_t *c, int on) {
    if (c->want_write == on) return;
    struct epoll_event ev = { .events = EPOLLIN | (on ? EPOLLOUT : 0), .data.fd = c->fd };
    epoll_ctl(srv.epfd, EPOLL_CTL_MOD, c->fd, &ev);
    c->want_write = on;
}

/* 응답 버퍼를 가능한 만큼 보낸다. 클라이언트를 해제했으면 -1. */
static int client_write(client_t *c) {
    while (c->reply_sent < c->reply.len) {
        ssize_t n = write(c->fd, c->reply.data + c->reply_sent, c->reply.len - c->reply_sent);
        if (n > 0) {
            c->reply_sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            /* 소켓 송신 버퍼가 가득 참 → 쓸 수 있게 되면 다시 깨워 달라고 등록 */
            client_set_write_interest(c, 1);
            return 0;
        } else {
            client_free(c); /* EPIPE 등: 상대가 끊었음 */
            return -1;
        }
    }

    /* 다 보냈음 */
    buffer_clear(&c->reply);
    c->reply_sent = 0;
    client_set_write_interest(c, 0);
    if (c->close_after_reply) {
        client_free(c);
        return -1;
    }
    return 0;
}

static void client_read(client_t *c) {
    buffer_reserve(&c->query, READ_CHUNK);
    ssize_t n = read(c->fd, c->query.data + c->query.len, READ_CHUNK);
    if (n == 0) {
        client_free(c); /* 상대가 연결을 닫음 (FR-008) */
        return;
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
        client_free(c);
        return;
    }
    c->query.len += (size_t)n;

    if (c->query.len > QUERY_BUF_LIMIT) {
        fprintf(stderr, "client fd=%d query buffer over limit, closing\n", c->fd);
        client_free(c);
        return;
    }

    /* 프로토콜 에러로 닫기로 한 클라이언트의 추가 입력은 무시한다. */
    if (!c->close_after_reply) client_process_input(c);

    if (c->reply.len > c->reply_sent) client_write(c);
}

static void accept_clients(void) {
    for (;;) {
        int fd = accept(srv.listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK) perror("accept");
            return; /* 대기 중인 연결을 모두 받음 */
        }
        if (fd >= srv.max_fd) {
            /* fd 한도를 넘음. Redis 처럼 에러를 보내고 닫는다. */
            const char *msg = "-ERR max number of clients reached\r\n";
            ssize_t ignored = write(fd, msg, strlen(msg));
            (void)ignored;
            close(fd);
            continue;
        }
        set_nonblocking(fd);
        int one = 1;
        /* 작은 응답을 모아서 보내지 않고 바로 보내도록 (Nagle 끄기) */
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        client_t *c = client_create(fd);
        struct epoll_event ev = { .events = EPOLLIN, .data.fd = fd };
        if (epoll_ctl(srv.epfd, EPOLL_CTL_ADD, fd, &ev) < 0) {
            perror("epoll_ctl");
            close(fd);
            free(c);
            continue;
        }
        srv.clients[fd] = c;
        if (srv.verbose) printf("client fd=%d connected\n", fd);
    }
}

/* ---------- 서버 ---------- */

int server_init(int port, int verbose) {
    srv.verbose = verbose;

    /* fd 를 인덱스로 쓰므로 프로세스의 fd 한도만큼 배열을 잡는다. */
    struct rlimit rl;
    srv.max_fd = 1024;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY) {
        srv.max_fd = rl.rlim_cur > 65536 ? 65536 : (int)rl.rlim_cur;
    }
    srv.clients = xcalloc((size_t)srv.max_fd, sizeof(client_t *));

    srv.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv.listen_fd < 0) {
        perror("socket");
        return -1;
    }
    int one = 1;
    /* 재시작 직후 TIME_WAIT 상태의 포트도 바로 다시 bind 할 수 있게 */
    setsockopt(srv.listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);

    if (bind(srv.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return -1;
    }
    if (listen(srv.listen_fd, 511) < 0) { /* Redis tcp-backlog 기본값 */
        perror("listen");
        return -1;
    }
    set_nonblocking(srv.listen_fd);

    srv.epfd = epoll_create1(0);
    if (srv.epfd < 0) {
        perror("epoll_create1");
        return -1;
    }
    struct epoll_event ev = { .events = EPOLLIN, .data.fd = srv.listen_fd };
    if (epoll_ctl(srv.epfd, EPOLL_CTL_ADD, srv.listen_fd, &ev) < 0) {
        perror("epoll_ctl");
        return -1;
    }
    return 0;
}

void server_request_stop(void) {
    srv.stop = 1;
}

void server_run(void) {
    struct epoll_event events[MAX_EVENTS];

    while (!srv.stop) {
        /* 시그널이 오면 EINTR 로 깨어나 stop 플래그를 확인한다. */
        int n = epoll_wait(srv.epfd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == srv.listen_fd) {
                accept_clients();
                continue;
            }

            /* 같은 epoll_wait 결과 안에서 앞선 이벤트 처리 중 해제됐을 수 있다. */
            client_t *c = srv.clients[fd];
            if (c == NULL) continue;

            if (ev & (EPOLLERR | EPOLLHUP)) {
                client_free(c);
                continue;
            }
            if (ev & EPOLLIN) {
                client_read(c);
                c = srv.clients[fd];
                if (c == NULL) continue;
            }
            if (ev & EPOLLOUT) client_write(c);
        }
    }
}

void server_shutdown(void) {
    for (int fd = 0; fd < srv.max_fd; fd++) {
        if (srv.clients[fd]) client_free(srv.clients[fd]);
    }
    free(srv.clients);
    srv.clients = NULL;
    if (srv.listen_fd >= 0) close(srv.listen_fd);
    if (srv.epfd >= 0) close(srv.epfd);
    srv.listen_fd = srv.epfd = -1;
}

/* T2 임시 구현: 받은 데이터를 버린다. T3 에서 RESP 파서(resp.c)로 교체한다. */
void client_process_input(client_t *c) {
    buffer_clear(&c->query);
    c->qpos = 0;
}
