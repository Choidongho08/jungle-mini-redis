#ifndef MINIREDIS_COMMAND_H
#define MINIREDIS_COMMAND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "miniredis/aof.h"
#include "miniredis/clock.h"
#include "miniredis/resp.h"
#include "miniredis/status.h"
#include "miniredis/store.h"

/*
 * 명령 디스패치 계층: argv 를 받아 저장소를 조작하고 RESP 응답을 out 에 덧붙인다.
 *
 * 소켓을 모르는 순수 함수 계층이라 네트워크 없이 단위 테스트할 수 있다.
 * 명령 테이블(이름·arity·핸들러·플래그)로 디스패치하며 이름은 대소문자를 무시한다.
 * arity 는 Redis 관례를 따른다: 양수 N 이면 정확히 N 개(명령 이름 포함), 음수 -N
 * 이면 N 개 이상.
 *
 * 영속화 순서: 쓰기 명령은 저장소 변경이 성공하면 AOF 에 정규형 레코드를 기록하고,
 * 기록이 성공한 뒤에야 성공 응답을 만든다. 따라서 클라이언트가 +OK 를 받았다면 그
 * 변경은 AOF 에 들어 있다(fsync 시점은 appendfsync 정책에 따름).
 * AOF 기록이 실패하면 메모리는 이미 바뀐 뒤라 되돌릴 수 없다(저장소에 undo 가 없음).
 * 그래서 오류를 응답하고 aof_write_failed 를 세워 이후 모든 쓰기 명령을 거부한다
 * (Redis 의 MISCONF 와 같은 정책). 메모리와 파일이 다시 일치하는 유일한 방법은
 * 현재 메모리 전체를 새로 쓰는 BGREWRITEAOF 이므로, 그것이 성공하면 해제한다.
 */

#define MR_SERVER_VERSION "1.0.0"

/* 서버가 채워 두는 읽기 전용 정보(INFO·CONFIG GET 용). 명령 계층은 카운터만 올린다. */
typedef struct mr_cmd_server_info {
    uint64_t total_commands;       /* 디스패치한 명령 수(mr_cmd_exec 가 증가) */
    uint64_t total_connections;    /* 수락한 연결 수 */
    uint64_t rejected_connections; /* maxclients 초과로 거절한 연결 수 */
    uint64_t connected_clients;    /* 현재 연결 수 */
    uint64_t total_net_input_bytes;
    uint64_t total_net_output_bytes;
    int64_t start_mono_ms; /* 업타임 계산 기준(clock 의 mono) */
    int port;
    const char *bind_addr;
    uint64_t maxclients;
    int64_t idle_timeout_sec;
    const char *appendfsync; /* "always" | "everysec" | "no" */
} mr_cmd_server_info;

typedef struct mr_cmd_env {
    mr_store *store;            /* 필수 */
    mr_aof *aof;                /* NULL 이면 AOF 비활성: 기록을 건너뛴다 */
    const mr_clock *clock;      /* 필수. 저장소와 같은 시계여야 TTL 계산이 일치한다 */
    mr_cmd_server_info *info;   /* NULL 허용(INFO 의 서버 섹션이 비게 됨) */
    bool aof_write_failed;      /* true 면 쓰기 명령 거부(위 설명) */
    uint64_t aof_rewrites;      /* 성공한 BGREWRITEAOF 수 */
    bool aof_last_rewrite_ok;
    int64_t aof_last_rewrite_ms; /* 마지막 rewrite 소요 시간(ms), 없으면 -1 */
} mr_cmd_env;

/* mr_cmd_exec 의 *flags 비트 */
#define MR_CMD_RESULT_CLOSE 0x1u /* 응답을 모두 보낸 뒤 연결을 닫아라(QUIT) */

/*
 * argv[0] 을 명령 이름으로 실행하고 응답 하나를 out 에 덧붙인다. argc 0 이면 아무것도
 * 하지 않는다. 명령 수준 오류(인자 수, 문법, 정수 아님 등)는 -ERR 응답이며 MR_OK 다.
 * MR_NOMEM/MR_OVERFLOW 는 out 에 응답을 완성하지 못했다는 뜻이므로 호출자는 연결을
 * 닫아야 한다. flags NULL 허용.
 */
mr_status mr_cmd_exec(mr_cmd_env *env, size_t argc, const mr_slice *argv, mr_buf *out,
                      unsigned *flags);

/* 명령 테이블 크기(COMMAND COUNT). */
size_t mr_cmd_count(void);

/* 이름(대소문자 무시)이 쓰기 명령이면 true. 없는 명령이면 false. */
bool mr_cmd_is_write(const void *name, size_t len);

#endif /* MINIREDIS_COMMAND_H */
