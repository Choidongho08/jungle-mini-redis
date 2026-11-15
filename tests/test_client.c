/* TCP 타이밍에 의존하지 않고 묶음 경계와 부분 전송을 검사한다. */
#include "server/client.h"
#include "common/net.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static Bytes bytes(const char *text)
{
    return (Bytes){(const unsigned char *)text, strlen(text)};
}

static Client make_client(const char *frame, size_t repeats, int *peer)
{
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(net_nonblocking(sockets[0]));
    assert(net_nonblocking(sockets[1]));
    *peer = sockets[1];
    size_t len = strlen(frame);
    Client client = {.fd = sockets[0], .end = len * repeats, .capacity = len * repeats};
    client.input = malloc(client.capacity);
    assert(client.input);
    for (size_t i = 0; i < repeats; i++)
        memcpy(client.input + i * len, frame, len);
    return client;
}

static void test_command_budget_and_reuse(void)
{
    const char *ping = "*1\r\n$4\r\nPING\r\n";
    int peer;
    Client client = make_client(ping, CLIENT_COMMAND_BUDGET + 1, &peer);
    Store *store = store_create();
    assert(store);
    client_prepare_reply(&client, store, NULL);
    assert(client.start == strlen(ping) * CLIENT_COMMAND_BUDGET);
    assert(client.reply.len == 7 * CLIENT_COMMAND_BUDGET);

    /* 출력이 남아 있는 동안 다음 명령을 실행하지 않는다. */
    size_t start = client.start;
    unsigned char *buffer = client.reply.data;
    client_prepare_reply(&client, store, NULL);
    assert(client.start == start);
    client_write(&client);
    assert(client.reply.len == 0 && client.sent == 0);
    assert(client.reply.data == buffer);

    unsigned char received[7 * CLIENT_COMMAND_BUDGET];
    assert(recv(peer, received, sizeof(received), 0) == (ssize_t)sizeof(received));
    for (size_t i = 0; i < CLIENT_COMMAND_BUDGET; i++)
        assert(memcmp(received + 7 * i, "+PONG\r\n", 7) == 0);

    client_prepare_reply(&client, store, NULL);
    assert(client.start == client.end);
    assert(client.reply.len == 7 && client.reply.data == buffer);
    assert(memcmp(client.reply.data, "+PONG\r\n", 7) == 0);
    client_close(&client);
    close(peer);
    store_destroy(store);
}

static void test_output_budget(void)
{
    const char *get = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
    int peer;
    Client client = make_client(get, CLIENT_COMMAND_BUDGET + 1, &peer);
    Store *store = store_create();
    assert(store);
    unsigned char value[4096];
    memset(value, 'v', sizeof(value));
    assert(store_set(store, bytes("k"), (Bytes){value, sizeof(value)}, 0, 0) == STORE_OK);
    client_prepare_reply(&client, store, NULL);
    assert(client.reply.len >= CLIENT_OUTPUT_HIGH_WATER);
    assert(client.reply.len <= CLIENT_OUTPUT_LIMIT);
    assert(client.reply_capacity <= CLIENT_OUTPUT_LIMIT);
    assert(client.start / strlen(get) < CLIENT_COMMAND_BUDGET);
    assert(client.start < client.end);
    client_close(&client);
    close(peer);
    store_destroy(store);
}

static void test_partial_write_preserves_data_and_execution_order(void)
{
    const char *frames = "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n"
                         "*2\r\n$3\r\nDEL\r\n$1\r\nk\r\n"
                         "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n";
    int peer;
    Client client = make_client(frames, 1, &peer);
    int send_size = 4096;
    assert(setsockopt(client.fd, SOL_SOCKET, SO_SNDBUF, &send_size, sizeof(send_size)) == 0);
    Store *store = store_create();
    assert(store);
    size_t size = 250000;
    unsigned char *value = malloc(size);
    assert(value);
    for (size_t i = 0; i < size; i++)
        value[i] = (unsigned char)i;
    Bytes data = {value, size};
    assert(store_set(store, bytes("k"), data, 0, 0) == STORE_OK);
    Reply expected = resp_bulk(data);
    assert(expected.data);
    client_prepare_reply(&client, store, NULL);
    assert(client.reply.len == expected.len);
    assert(client.reply_capacity <= CLIENT_OUTPUT_LIMIT);

    client_write(&client);
    assert(client.sent > 0 && client.sent < client.reply.len);
    size_t start = client.start;
    size_t sent = client.sent;
    client_prepare_reply(&client, store, NULL);
    assert(client.start == start && client.sent == sent);
    Bytes found;
    assert(store_get(store, bytes("k"), 0, &found));

    unsigned char *received = malloc(expected.len);
    assert(received);
    size_t total = 0;
    size_t iterations = 0;
    while (client.reply.len || total < expected.len) {
        assert(++iterations < 10000);
        if (client.reply.len)
            client_write(&client);
        if (total < expected.len) {
            ssize_t count = recv(peer, received + total, expected.len - total, 0);
            if (count > 0)
                total += (size_t)count;
            else
                assert(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        }
    }
    assert(memcmp(received, expected.data, expected.len) == 0);
    client_prepare_reply(&client, store, NULL);
    const char *tail = ":1\r\n$-1\r\n";
    assert(client.reply.len == strlen(tail));
    assert(memcmp(client.reply.data, tail, strlen(tail)) == 0);
    assert(!store_get(store, bytes("k"), 0, &found));
    free(received);
    free(expected.data);
    free(value);
    client_close(&client);
    close(peer);
    store_destroy(store);
}

int main(void)
{
    test_command_budget_and_reuse();
    test_output_budget();
    test_partial_write_preserves_data_and_execution_order();
    puts("Client batching, backpressure, and partial-write tests passed");
    return 0;
}
