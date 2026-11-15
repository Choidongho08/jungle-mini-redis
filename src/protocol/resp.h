#ifndef RESP_H
#define RESP_H

#include "storage/store.h"

#define MAX_ARGS 64
#define MAX_REQUEST (256u * 1024u)

/* args는 입력 버퍼를 참조한다. consumed는 이번 요청이 차지한 바이트 수다. */
typedef struct {
    Bytes args[MAX_ARGS];
    size_t count;
    size_t consumed;
} Request;

typedef enum {
    RESP_INCOMPLETE, /* 데이터가 더 오면 다시 파싱해야 함. */
    RESP_INVALID, /* 지원하는 요청 형식 또는 제한에 맞지 않음. */
    RESP_COMPLETE /* 명령 하나의 파싱 완료. */
} ParseResult;

/* 응답 함수가 할당한다. 호출자가 출력 버퍼에 복사하거나 전송한 뒤 free한다. */
typedef struct {
    unsigned char *data;
    size_t len;
} Reply;

ParseResult resp_parse(const unsigned char *data, size_t len, Request *request);
Reply resp_text(const char *text);
Reply resp_bulk(Bytes value);
Reply resp_integer(int64_t value);
bool parse_number(Bytes input, int64_t limit, int64_t *value);

#endif
