#ifndef MINI_REDIS_SERVER_H
#define MINI_REDIS_SERVER_H

#include <stddef.h>

#include "buffer.h"

/* 접속한 클라이언트 하나의 상태 (design §5.1).
 * 단일 스레드라 이 구조체에 락이 없다. */
typedef struct client {
    int fd;

    /* 입력: 받은 바이트와 파싱이 끝난 위치 */
    buffer_t query;
    size_t qpos;

    /* RESP 파서 상태. 요청이 여러 read 에 나뉘어 도착해도 여기서 이어간다 (FR-004). */
    long multibulk_left; /* 아직 못 받은 인자 수. 0 이면 새 요청 시작 */
    long bulk_len;       /* 현재 인자의 길이. -1 이면 "$<len>" 줄을 기다리는 중 */
    int argc;
    int argv_cap;
    bytes_t *argv;

    /* 출력 */
    buffer_t reply;
    size_t reply_sent;
    int want_write;        /* EPOLLOUT 등록 여부 */
    int close_after_reply; /* 프로토콜 에러 후 응답만 보내고 연결 종료 */
} client_t;

/* listen 소켓과 epoll 을 준비한다. 실패하면 -1. */
int server_init(int port, int verbose);

/* 이벤트 루프. server_request_stop() 이 호출될 때까지 돈다. */
void server_run(void);

/* 시그널 핸들러에서 호출해도 안전하다 (플래그만 세운다). */
void server_request_stop(void);

/* 모든 클라이언트와 소켓을 해제한다. */
void server_shutdown(void);

/* 입력 버퍼에 새 데이터가 들어왔을 때 호출된다.
 * 완성된 요청을 파싱/실행하고 응답을 c->reply 에 쌓는다. (resp.c 에서 구현) */
void client_process_input(client_t *c);

#endif
