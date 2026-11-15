/* 진입점: 실행 인자 검증, 리스너·저장소 생성 및 최종 자원 정리. */
#include "server/server.h"
#include "common/net.h"
#include "protocol/resp.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "127.0.0.1";
    const char *port_text = argc > 2 ? argv[2] : "6379";
    int64_t port;
    struct sockaddr_in address = {.sin_family = AF_INET};
    if (argc > 4 || inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
        !parse_number((Bytes){(const unsigned char *)port_text, strlen(port_text)}, 65535, &port)) {
        fprintf(stderr, "Usage: %s [IPv4 address] [port] [data directory]\n", argv[0]);
        return EXIT_FAILURE;
    }
    /* TCP/IP 포트 필드는 네트워크 바이트 순서로 저장한다. */
    address.sin_port = htons((uint16_t)port);

    if (!server_install_signal_handlers())
        return EXIT_FAILURE;

    Store *store = store_create();
    if (!store) {
        perror("store_create");
        return EXIT_FAILURE;
    }

    Persistence *persistence = NULL;
    if (argc == 4 && !(persistence = persistence_open(argv[3], store))) {
        perror("persistence recovery");
        store_destroy(store);
        return EXIT_FAILURE;
    }

    int result = EXIT_FAILURE;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    socklen_t address_len = sizeof(address);
    /* socket 생성 → 주소 바인딩 → 연결 대기 시작 → 이벤트 루프 진입.
     * 포트 0을 지정한 경우 getsockname으로 실제 배정된 포트를 알아낸다. */
    if (listener < 0 || !net_nonblocking(listener) ||
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0 ||
        bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listener, MAX_CLIENTS) < 0 ||
        getsockname(listener, (struct sockaddr *)&address, &address_len) < 0) {
        perror("listen setup");
    } else {
        printf("Listening on %s:%u\n", host, (unsigned)ntohs(address.sin_port));
        fflush(stdout);
        result = server_run(listener, store, persistence);
        if (result == EXIT_SUCCESS && persistence && !persistence_save(persistence, store)) {
            perror("snapshot on shutdown");
            result = EXIT_FAILURE;
        }
    }

    if (listener >= 0)
        close(listener);
    persistence_close(persistence);
    store_destroy(store);
    return result;
}
