/* RESP2 변환 계층: 입력 버퍼를 명령 인자로 해석하고 응답 바이트를 만든다. */
#include "protocol/resp.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 부호 없는 10진 숫자만 허용한다. 곱셈 전에 limit을 검사해 오버플로를 막는다. */
bool parse_number(Bytes input, int64_t limit, int64_t *value)
{
    if (input.len == 0)
        return false;

    int64_t number = 0;
    for (size_t i = 0; i < input.len; i++) {
        int digit = input.data[i] - '0';
        if (digit < 0 || digit > 9 || number > limit / 10 ||
            (number == limit / 10 && digit > limit % 10))
            return false;
        number = number * 10 + digit;
    }
    *value = number;
    return true;
}

/* *인자수\r\n 또는 $바이트수\r\n을 읽고 pos를 본문 시작으로 옮긴다.
 * TCP에서는 데이터가 나뉘어 올 수 있어 미완성과 잘못된 형식을 구분한다. */
static ParseResult read_length(const unsigned char *data, size_t len, size_t *pos,
                               unsigned char prefix, int64_t limit, int64_t *number)
{
    if (*pos == len)
        return RESP_INCOMPLETE;
    if (data[(*pos)++] != prefix)
        return RESP_INVALID;

    size_t start = *pos;
    while (*pos < len && data[*pos] >= '0' && data[*pos] <= '9') {
        if (*pos - start == 19)
            return RESP_INVALID;
        (*pos)++;
    }

    if (*pos == len)
        return RESP_INCOMPLETE;
    if (data[*pos] != '\r' ||
        !parse_number((Bytes){data + start, *pos - start}, limit, number))
        return RESP_INVALID;
    if (len - *pos < 2)
        return RESP_INCOMPLETE;
    if (data[*pos + 1] != '\n')
        return RESP_INVALID;
    *pos += 2;
    return RESP_COMPLETE;
}

/* 요청 형식은 벌크 문자열들의 배열이다. 예: *2\r\n$3\r\nGET\r\n$1\r\nk\r\n
 * 미완성 요청은 호출자가 버퍼에 데이터를 더 받은 뒤 처음부터 다시 파싱한다. */
ParseResult resp_parse(const unsigned char *data, size_t len, Request *request)
{
    size_t pos = 0;
    int64_t count;
    ParseResult status = read_length(data, len, &pos, '*', MAX_ARGS, &count);
    if (status != RESP_COMPLETE)
        return status;
    if (count == 0)
        return RESP_INVALID;

    request->count = (size_t)count;
    for (size_t i = 0; i < request->count; i++) {
        int64_t length;
        status = read_length(data, len, &pos, '$', MAX_REQUEST, &length);
        if (status != RESP_COMPLETE)
            return status;

        /* 값 안의 CRLF가 아닌 길이로 본문의 끝을 찾는다. */
        size_t end = pos + (size_t)length;
        if (end + 2 > MAX_REQUEST)
            return RESP_INVALID;
        if (len < end + 2)
            return RESP_INCOMPLETE;
        if (data[end] != '\r' || data[end + 1] != '\n')
            return RESP_INVALID;

        /* 인자를 복사하지 않고 입력 버퍼 안의 범위를 가리킨다. */
        request->args[i] = (Bytes){data + pos, (size_t)length};
        pos = end + 2;
    }

    /* 뒤에 다른 명령이 붙어 있어도 이번 요청이 차지한 바이트 수만 반환한다. */
    request->consumed = pos;
    return RESP_COMPLETE;
}

/* 이미 RESP 형식을 갖춘 텍스트를 복사한다. 반환 버퍼는 호출자가 사용 후 해제한다. */
Reply resp_text(const char *text)
{
    size_t len = strlen(text);
    unsigned char *data = malloc(len);
    if (data)
        memcpy(data, text, len);
    return (Reply){data, len};
}

/* 벌크 응답: $길이\r\n + 원본 바이트 + \r\n. 빈 값도 길이 0으로 표현한다.
 * 값을 별도 응답 버퍼에 복사하므로 이후 저장소가 바뀌어도 전송할 내용은 유지된다. */
Reply resp_bulk(Bytes value)
{
    char header[32];
    size_t header_len = (size_t)snprintf(header, sizeof(header), "$%zu\r\n", value.len);
    size_t len = header_len + value.len + 2;
    unsigned char *data = malloc(len);
    if (data) {
        memcpy(data, header, header_len);
        if (value.len)
            memcpy(data + header_len, value.data, value.len);
        memcpy(data + len - 2, "\r\n", 2);
    }
    return (Reply){data, len};
}

/* 정수 응답은 :숫자\r\n 형식이며 음수도 표현할 수 있다. */
Reply resp_integer(int64_t value)
{
    char text[32];
    snprintf(text, sizeof(text), ":%" PRId64 "\r\n", value);
    return resp_text(text);
}
