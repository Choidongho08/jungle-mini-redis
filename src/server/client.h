#ifndef CLIENT_H
#define CLIENT_H

#include "protocol/resp.h"
#include "persistence/persistence.h"

/* 연결당 한 루프의 작업량과 출력 메모리를 제한한다.
 * 응답 하나를 추가한 뒤 high-water를 검사하므로 최대 응답 한 개만큼 초과할 수 있다. */
#define CLIENT_COMMAND_BUDGET 32
#define CLIENT_OUTPUT_HIGH_WATER (64u * 1024u)
#define CLIENT_OUTPUT_LIMIT (CLIENT_OUTPUT_HIGH_WATER + MAX_REQUEST + 64u)

/* 연결 하나의 상태. TCP 수신 단위와 명령 단위가 다르므로 버퍼가 필요하다. */
typedef struct {
    int fd; /* 소켓 파일 디스크립터. -1이면 비어 있는 연결 슬롯. */
    unsigned char *input;
    /* 미처리 입력 범위는 [start, end). 처리한 앞부분은 다음 읽기 때 정리한다. */
    size_t start;
    size_t end;
    size_t capacity;
    Reply reply; /* 실행 순서대로 인코딩한 응답들의 출력 버퍼. len 0이면 전송할 내용 없음. */
    size_t reply_capacity; /* 전송 완료 후에도 할당을 유지해 다음 묶음에서 재사용한다. */
    size_t sent; /* 부분 전송 후 다음에 보낼 바이트 위치. */
    bool close_after_reply; /* 잘못된 요청이면 오류 응답을 보낸 뒤 연결 종료. */
    int64_t last_io;
} Client;

/* 초기 슬롯은 fd = -1로 둔다. 모든 함수는 이벤트 루프 스레드에서 호출한다. */
void client_close(Client *client);
void client_prepare_reply(Client *client, Store *store, Persistence *persistence);
void client_read(Client *client);
void client_write(Client *client);
void client_accept(int listener, Client clients[], size_t count);

#endif
