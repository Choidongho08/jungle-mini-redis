/* 명령 계층: RESP 요청을 저장소 연산과 응답으로 변환한다. */
#include "commands/command.h"

#include <string.h>

/* 명령 이름과 옵션은 ASCII 대소문자를 구분하지 않는다. 키·값은 그대로 다룬다. */
static bool matches(Bytes input, const char *upper)
{
    if (input.len != strlen(upper))
        return false;
    for (size_t i = 0; i < input.len; i++) {
        unsigned char ch = input.data[i];
        if (ch >= 'a' && ch <= 'z')
            ch -= 'a' - 'A';
        if (ch != (unsigned char)upper[i])
            return false;
    }
    return true;
}

static Reply set_command(Store *store, const Request *request, int64_t now)
{
    int64_t deadline = 0; /* TTL 옵션 없는 SET은 기존 키의 TTL도 제거한다. */
    const Bytes *args = request->args;
    if (request->count == 5) {
        int64_t unit;
        if (matches(args[3], "PX"))
            unit = 1;
        else if (matches(args[3], "EX"))
            unit = 1000;
        else
            return resp_text("-ERR syntax error\r\n");

        /* 초(EX)도 밀리초로 통일한다. 현재 시각을 더할 공간까지 고려해 검증한다. */
        int64_t ttl;
        if (!parse_number(args[4], (INT64_MAX - now) / unit, &ttl) || ttl == 0)
            return resp_text("-ERR invalid expire time\r\n");
        deadline = now + ttl * unit;
    } else if (request->count != 3) {
        return resp_text("-ERR wrong number of arguments\r\n");
    }

    StoreResult result = store_set(store, args[1], args[2], deadline, now);
    if (result == STORE_FULL)
        return resp_text("-OOM store limit reached\r\n");
    if (result == STORE_NO_MEMORY)
        return resp_text("-OOM allocation failed\r\n");
    return resp_text("+OK\r\n");
}

/* 파싱된 명령을 저장소 호출로 연결하고 RESP 응답을 만든다.
 * 이 함수 실행 중에는 다른 연결의 명령이나 만료 정리가 끼어들지 않는다. */
Reply command_execute(Store *store, const Request *request, int64_t now,
                         Persistence *persistence)
{
    const Bytes *args = request->args;
    size_t count = request->count;
    if (matches(args[0], "SET"))
        return set_command(store, request, now);
    if (persistence && matches(args[0], "SAVE")) {
        if (count != 1)
            return resp_text("-ERR wrong number of arguments\r\n");
        return resp_text(persistence_save(persistence, store) ?
                         "+OK\r\n" : "-ERR snapshot failed\r\n");
    }

    if (matches(args[0], "PING")) {
        if (count == 1)
            return resp_text("+PONG\r\n");
        if (count == 2)
            return resp_bulk(args[1]);
    } else if (matches(args[0], "GET")) {
        if (count == 2) {
            Bytes value;
            return store_get(store, args[1], now, &value) ?
                resp_bulk(value) : resp_text("$-1\r\n");
        }
    } else if (matches(args[0], "PTTL")) {
        if (count == 2)
            return resp_integer(store_pttl(store, args[1], now));
    } else if (matches(args[0], "DEL")) {
        if (count >= 2) {
            int64_t deleted = 0;
            /* 삭제에 성공한 키만 센다. 같은 키를 반복 지정해도 중복 집계하지 않는다. */
            for (size_t i = 1; i < count; i++)
                deleted += store_delete(store, args[i], now);
            return resp_integer(deleted);
        }
    } else {
        return resp_text("-ERR unknown command\r\n");
    }

    return resp_text("-ERR wrong number of arguments\r\n");
}
