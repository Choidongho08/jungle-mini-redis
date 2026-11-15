/* 연결 계층: 입력·응답 버퍼와 부분 수신·전송 상태를 관리한다. */
#include "server/client.h"
#include "commands/command.h"
#include "common/net.h"
#include "common/time_utils.h"

#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* 연결이 소유한 소켓, 입력 버퍼, 응답 버퍼를 정리하고 슬롯을 재사용 가능하게 한다. */
void client_close(Client *client)
{
    close(client->fd);
    free(client->input);
    free(client->reply.data);
    *client = (Client){.fd = -1};
}

/* 응답의 소유권을 받아 출력 버퍼에 복사한 뒤 임시 응답을 해제한다. */
static bool append_reply(Client *client, Reply reply)
{
    if (!reply.data)
        return false;

    if (reply.len > CLIENT_OUTPUT_LIMIT - client->reply.len) {
        free(reply.data);
        return false;
    }

    size_t needed = client->reply.len + reply.len;
    if (needed > client->reply_capacity) {
        size_t capacity = client->reply_capacity ? client->reply_capacity : 4096;
        while (capacity < needed)
            capacity *= 2;
        if (capacity > CLIENT_OUTPUT_LIMIT)
            capacity = CLIENT_OUTPUT_LIMIT;
        unsigned char *data = realloc(client->reply.data, capacity);
        if (!data) {
            free(reply.data);
            return false;
        }
        client->reply.data = data;
        client->reply_capacity = capacity;
    }
    memcpy(client->reply.data + client->reply.len, reply.data, reply.len);
    client->reply.len = needed;
    free(reply.data);
    return true;
}

void client_prepare_reply(Client *client, Store *store, Persistence *persistence)
{
    /* 부분 전송 중에는 버퍼를 변경하지 않는다. 출력이 비워질 때까지 실행을 보류한다. */
    if (client->reply.len || client->close_after_reply)
        return;

    for (size_t commands = 0;
         commands < CLIENT_COMMAND_BUDGET &&
         client->reply.len < CLIENT_OUTPUT_HIGH_WATER && client->start < client->end;
         commands++) {
        Request request;
        size_t len = client->end - client->start;
        ParseResult status = resp_parse(client->input + client->start, len, &request);
        /* 미완성 요청은 보존하고, 이미 만든 응답부터 전송한다. */
        if (status == RESP_INCOMPLETE && len < MAX_REQUEST)
            break;

        Reply reply;
        if (status == RESP_COMPLETE) {
            reply = command_execute(store, &request, now_ms(), persistence);
            client->start += request.consumed;
        } else {
            reply = resp_text("-ERR invalid RESP request\r\n");
            client->close_after_reply = true;
        }

        if (!append_reply(client, reply)) {
            client_close(client);
            return;
        }
        if (client->close_after_reply)
            break;
    }
}

/* 시그널 중단 또는 일시적인 준비 부족이면 연결을 유지하고 다음 루프에서 재시도한다. */
static bool retry_io(void)
{
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK;
}

void client_read(Client *client)
{
    /* 미처리 바이트를 앞으로 모은다. 영역이 겹칠 수 있으므로 memmove를 쓴다. */
    if (client->start) {
        client->end -= client->start;
        memmove(client->input, client->input + client->start, client->end);
        client->start = 0;
    }

    /* 필요한 만큼 두 배씩 늘리되 연결 입력 버퍼를 요청 최대 크기로 제한한다. */
    if (client->end == client->capacity) {
        size_t capacity = client->capacity ? client->capacity * 2 : 4096;
        if (capacity > MAX_REQUEST)
            capacity = MAX_REQUEST;
        unsigned char *input = realloc(client->input, capacity);
        if (!input) {
            client_close(client);
            return;
        }
        client->input = input;
        client->capacity = capacity;
    }

    /* recv 한 번에 요청 일부만 오거나 여러 요청이 함께 올 수 있다. */
    ssize_t read_bytes = recv(client->fd, client->input + client->end,
                              client->capacity - client->end, 0);
    if (read_bytes > 0) {
        client->end += (size_t)read_bytes;
        client->last_io = now_ms();
    } else if (read_bytes == 0 || !retry_io()) {
        client_close(client);
    }
}

void client_write(Client *client)
{
    /* send도 일부만 보낼 수 있다. sent 이후만 보내고 나머지는 POLLOUT 때 이어간다. */
    ssize_t written = send(client->fd, client->reply.data + client->sent,
                           client->reply.len - client->sent, 0);
    if (written <= 0) {
        if (written == 0 || !retry_io())
            client_close(client);
        return;
    }

    client->last_io = now_ms();
    client->sent += (size_t)written;
    if (client->sent == client->reply.len) {
        if (client->close_after_reply) {
            client_close(client);
            return;
        }
        client->reply.len = 0;
        client->sent = 0;
    }
}

void client_accept(int listener, Client clients[], size_t count)
{
    int fd = accept(listener, NULL, NULL);
    if (fd < 0)
        return;

    /* TCP_NODELAY로 작은 응답을 모아서 보내려는 지연을 줄인다. */
    int yes = 1;
    if (net_nonblocking(fd) &&
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == 0) {
        for (size_t i = 0; i < count; i++) {
            if (clients[i].fd < 0) {
                clients[i] = (Client){.fd = fd, .last_io = now_ms()};
                return;
            }
        }
    }
    /* 설정 실패 또는 연결 슬롯 부족이면 새 연결을 종료한다. */
    close(fd);
}
