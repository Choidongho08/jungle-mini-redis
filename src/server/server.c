/* 단일 이벤트 루프: 명령 실행, 소켓 I/O, TTL 정리를 순서대로 수행한다. */
#include "server/server.h"
#include "server/client.h"
#include "common/time_utils.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#define SWEEP_INTERVAL_MS 100
#define SWEEP_BUCKETS 128
#define IDLE_TIMEOUT_MS 60000

/* 시그널 핸들러는 종료 표시만 하고, 실제 자원 정리는 이벤트 루프 밖에서 한다. */
static volatile sig_atomic_t stopping;

static void stop_server(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

bool server_install_signal_handlers(void)
{
    /* Ctrl+C와 종료 요청은 정상 정리로 연결한다. 끊긴 소켓 쓰기는
     * SIGPIPE로 프로세스를 종료하는 대신 send 오류로 처리한다. */
    struct sigaction action = {.sa_handler = stop_server};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0 ||
        signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal setup");
        return false;
    }

    return true;
}

/* 같은 스레드가 소켓 I/O, 명령 실행, TTL 정리를 모두 담당한다. */
int server_run(int listener, Store *store, Persistence *persistence)
{
    Client clients[MAX_CLIENTS];
    struct pollfd fds[MAX_CLIENTS + 1];
    /* 0번은 새 연결을 받는 리스너, i+1번은 clients[i]에 대응한다. */
    fds[0] = (struct pollfd){.fd = listener, .events = POLLIN};
    for (size_t i = 0; i < MAX_CLIENTS; i++)
        clients[i] = (Client){.fd = -1};

    int result = EXIT_SUCCESS;
    int64_t next_sweep = now_ms() + SWEEP_INTERVAL_MS;
    while (!stopping) {
        int64_t now = now_ms();
        if (now >= next_sweep) {
            store_sweep(store, now, SWEEP_BUCKETS);
            next_sweep = now + SWEEP_INTERVAL_MS;
        }

        /* 연결마다 명령 수·출력량을 제한한 묶음만 실행해 독점을 방지한다. */
        for (size_t i = 0; i < MAX_CLIENTS; i++) {
            Client *client = &clients[i];
            if (client->fd >= 0) {
                if (now - client->last_io >= IDLE_TIMEOUT_MS)
                    client_close(client);
                else
                    client_prepare_reply(client, store, persistence);
            }
            fds[i + 1] = (struct pollfd){
                .fd = client->fd,
                /* 응답이 있으면 쓰기, 없으면 읽기 준비만 감시한다. */
                .events = client->reply.len ? POLLOUT : POLLIN,
            };
        }

        /* 요청이 없어도 다음 만료 정리 시점에는 poll에서 깨어나도록 한다. */
        int64_t remaining = next_sweep - now_ms();
        int timeout = remaining > 0 ? (int)remaining : 0;
        if (poll(fds, MAX_CLIENTS + 1, timeout) < 0) {
            if (errno == EINTR)
                continue;
            perror("poll");
            result = EXIT_FAILURE;
            break;
        }

        /* poll이 알려 준 준비 상태에 맞는 I/O만 수행한다. */
        for (size_t i = 0; i < MAX_CLIENTS; i++) {
            Client *client = &clients[i];
            short events = fds[i + 1].revents;
            if (events & (POLLERR | POLLNVAL))
                client_close(client);
            else if (client->reply.len && (events & (POLLOUT | POLLHUP)))
                client_write(client);
            else if (events & (POLLIN | POLLHUP))
                client_read(client);
        }
        if (fds[0].revents & POLLIN)
            client_accept(listener, clients, MAX_CLIENTS);
    }

    for (size_t i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0)
            client_close(&clients[i]);
    }
    return result;
}
