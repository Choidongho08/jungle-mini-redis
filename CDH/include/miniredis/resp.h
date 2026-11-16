#ifndef MINIREDIS_RESP_H
#define MINIREDIS_RESP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "miniredis/status.h"

/*
 * RESP2(Redis Serialization Protocol v2) 요청 파서와 응답 직렬화.
 *
 * 요청 형식 두 가지를 받는다.
 *  - multibulk: *<n>\r\n $<len>\r\n<bytes>\r\n ...   (redis-cli·클라이언트 라이브러리)
 *  - inline   : 공백/탭으로 구분한 한 줄 텍스트 + \n   (telnet·nc 편의)
 *
 * 스트리밍 설계
 *  - 파서는 "어디까지 읽었는가"를 상태로 들고 있어 바이트가 한 개씩 와도 이미 본
 *    부분을 다시 해석하지 않는다(인자 수가 많은 명령을 잘게 보내는 공격에도 O(n)).
 *  - 인자는 입력 버퍼 내 "오프셋"으로 기억한다. 호출자는 명령이 끝나기 전까지
 *    버퍼를 realloc 하거나 앞쪽(아직 소비 안 된 시작점 이전)을 지워 당겨도 된다.
 *    포인터를 기억했다면 realloc 한 번에 모두 무효가 되었을 것이다.
 *  - 명령이 완성되면 그 순간의 버퍼 시작 주소로 argv(포인터+길이)를 만들어 준다.
 *    argv 는 다음 mr_resp_parse 호출 또는 버퍼 변경 전까지만 유효하다(빌린 포인터).
 *
 * 메모리 DoS 방지
 *  - bulk 길이, multibulk 인자 수, 한 줄(헤더/inline) 길이에 상한을 둔다. 선언된
 *    인자 수만큼 미리 할당하지 않고 실제로 도착한 만큼만 키운다("*1000000" 한 줄로
 *    수십 MB 를 할당시키는 것을 막는다).
 *  - 상한 초과·문법 위반은 복구 불가능한 프로토콜 오류다. 스트림의 경계를 더는
 *    신뢰할 수 없으므로 호출자는 오류를 보낸 뒤 연결을 닫아야 한다(Redis 동일).
 */

/* 길이 기반 바이트열 조각(빌린 포인터). NUL 을 포함할 수 있다. */
typedef struct mr_slice {
    const unsigned char *ptr;
    size_t len;
} mr_slice;

/* ---------- 가변 바이트 버퍼 ---------- */

typedef struct mr_buf {
    unsigned char *data;
    size_t len;
    size_t cap;
} mr_buf;

/* 0 으로 초기화된 mr_buf 도 유효한 빈 버퍼다. */
void mr_buf_init(mr_buf *b);
void mr_buf_free(mr_buf *b);
/* len 뒤에 extra 바이트 이상 여유를 확보. MR_OK / MR_NOMEM / MR_OVERFLOW. */
mr_status mr_buf_reserve(mr_buf *b, size_t extra);
mr_status mr_buf_append(mr_buf *b, const void *p, size_t n);

/* ---------- 응답 직렬화 ---------- */

/*
 * 모두 b 끝에 덧붙인다. 실패(MR_NOMEM/MR_OVERFLOW) 시 b 는 일부만 쓰였을 수 있으므로
 * 호출자는 그 연결을 닫아야 한다(반쪽 응답을 보낼 수는 없다).
 * simple/error 문자열 안의 \r \n 은 공백으로 바꾼다: 한 줄 형식이 깨지면 이후
 * 응답 전체의 경계가 어긋나기 때문이다.
 */
mr_status mr_resp_simple(mr_buf *b, const char *s);   /* +s\r\n */
mr_status mr_resp_error(mr_buf *b, const char *msg);  /* -msg\r\n (msg 에 "ERR " 등 접두어 포함) */
mr_status mr_resp_errorf(mr_buf *b, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
mr_status mr_resp_int(mr_buf *b, int64_t v);          /* :v\r\n */
mr_status mr_resp_bulk(mr_buf *b, const void *p, size_t n); /* $n\r\n<p>\r\n */
mr_status mr_resp_bulk_cstr(mr_buf *b, const char *s);
mr_status mr_resp_null(mr_buf *b);                    /* $-1\r\n */
mr_status mr_resp_array(mr_buf *b, size_t n);         /* *n\r\n */

/* ---------- 요청 파서 ---------- */

typedef struct mr_resp_limits {
    size_t max_bulk_len;   /* 인자 하나의 최대 바이트 수 */
    size_t max_args;       /* 명령 하나의 최대 인자 수 */
    size_t max_inline_len; /* inline 명령·헤더 한 줄의 최대 길이 */
} mr_resp_limits;

#define MR_RESP_DEFAULT_MAX_BULK ((size_t)64 * 1024 * 1024)
#define MR_RESP_DEFAULT_MAX_ARGS ((size_t)1024 * 1024)
#define MR_RESP_DEFAULT_MAX_INLINE ((size_t)64 * 1024)

typedef enum mr_resp_result {
    MR_RESP_NEED_MORE = 0, /* 명령이 아직 덜 왔다. 더 받아서 같은 시작점으로 다시 호출 */
    MR_RESP_COMMAND,       /* 명령 완성: argc/argv 사용 가능, *consumed 바이트 소비 */
    MR_RESP_ERROR,         /* 프로토콜 오류(복구 불가). mr_resp_error_msg 참고 */
    MR_RESP_NOMEM          /* 인자 배열 할당 실패 */
} mr_resp_result;

typedef struct mr_resp_span {
    size_t off;
    size_t len;
} mr_resp_span;

/* 필드는 내부 상태다. 스택/구조체에 직접 두기 위해 공개할 뿐 직접 만지지 말 것. */
typedef struct mr_resp_parser {
    mr_resp_limits lim;
    int state;          /* 내부 상태 머신 */
    size_t pos;         /* 현재 명령 시작점 기준으로 해석을 마친 위치 */
    size_t scan;        /* 줄 끝 탐색을 이어 갈 위치(같은 바이트 재탐색 방지) */
    size_t need;        /* multibulk 에서 선언된 인자 수 */
    size_t bulk_len;    /* 읽는 중인 bulk 길이 */
    size_t argc;
    size_t cap;
    mr_resp_span *spans;
    mr_slice *argv;
    const char *err;
} mr_resp_parser;

/* lim NULL 이면 기본 상한. 0 인 필드도 기본값으로 채운다. */
void mr_resp_parser_init(mr_resp_parser *p, const mr_resp_limits *lim);
void mr_resp_parser_free(mr_resp_parser *p);

/*
 * buf[0..len) 은 "아직 소비되지 않은 입력의 시작"부터의 바이트여야 한다(명령이 덜
 * 왔다면 다음 호출에도 같은 시작점에 이어 붙인 버퍼를 넘긴다).
 * MR_RESP_COMMAND 이면 *consumed 에 그 명령의 바이트 수. argc 가 0 일 수 있다
 * (빈 inline 줄, "*0", "*-1"): 호출자는 소비만 하고 무시한다.
 * ERROR 상태는 고착된다(init 전까지 계속 ERROR).
 */
mr_resp_result mr_resp_parse(mr_resp_parser *p, const unsigned char *buf, size_t len,
                             size_t *consumed);

size_t mr_resp_argc(const mr_resp_parser *p);
const mr_slice *mr_resp_argv(const mr_resp_parser *p);
/* "Protocol error: ..." 형식의 정적 문자열. 오류가 없으면 NULL. */
const char *mr_resp_error_msg(const mr_resp_parser *p);

/*
 * Redis string2ll 과 같은 엄격한 10진 정수 해석(앞뒤 공백·'+'·선행 0·"-0" 거부).
 * 명령 인자 검증과 프로토콜 헤더 해석이 같은 규칙을 쓰도록 공개한다.
 */
bool mr_parse_i64(const void *p, size_t len, int64_t *out);

#endif /* MINIREDIS_RESP_H */
