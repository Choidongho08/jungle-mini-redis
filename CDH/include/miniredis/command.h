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
 * 지원 명령(9개): PING, SET key value [EX seconds], GET, DEL, EXISTS, EXPIRE, TTL,
 * INCR, FLUSHALL. 그 밖의 이름은 unknown command 오류다.
 *
 * 영속화 순서: 쓰기 명령은 저장소 변경이 성공하면 AOF 에 정규형 레코드를 기록하고,
 * 기록이 성공한 뒤에야 성공 응답을 만든다. 따라서 클라이언트가 +OK 를 받았다면 그
 * 변경은 AOF 에 들어 있다(fsync 시점은 appendfsync 정책에 따름).
 * AOF 기록이 실패하면 메모리는 이미 바뀐 뒤라 되돌릴 수 없다(저장소에 undo 가 없음).
 * 그래서 오류를 응답하고 aof_write_failed 를 세워 이후 모든 쓰기 명령을 거부한다
 * (Redis 의 MISCONF 와 같은 정책). 이 플래그를 내리는 명령은 없다: 복구는 서버 재시작
 * (AOF 재생)뿐이다.
 */

#define MR_SERVER_VERSION "1.0.0"

typedef struct mr_cmd_env {
    mr_store *store;            /* 필수 */
    mr_aof *aof;                /* NULL 이면 AOF 비활성: 기록을 건너뛴다 */
    const mr_clock *clock;      /* 필수. 저장소와 같은 시계여야 TTL 계산이 일치한다 */
    bool aof_write_failed;      /* true 면 쓰기 명령 거부(위 설명) */
} mr_cmd_env;

/*
 * argv[0] 을 명령 이름으로 실행하고 응답 하나를 out 에 덧붙인다. argc 0 이면 아무것도
 * 하지 않는다. 명령 수준 오류(인자 수, 문법, 정수 아님 등)는 -ERR 응답이며 MR_OK 다.
 * MR_NOMEM/MR_OVERFLOW 는 out 에 응답을 완성하지 못했다는 뜻이므로 호출자는 연결을
 * 닫아야 한다.
 */
mr_status mr_cmd_exec(mr_cmd_env *env, size_t argc, const mr_slice *argv, mr_buf *out);

/* 명령 테이블 크기. */
size_t mr_cmd_count(void);

/* 이름(대소문자 무시)이 쓰기 명령이면 true. 없는 명령이면 false. */
bool mr_cmd_is_write(const void *name, size_t len);

#endif /* MINIREDIS_COMMAND_H */
