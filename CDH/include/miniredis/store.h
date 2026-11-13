#ifndef MINIREDIS_STORE_H
#define MINIREDIS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "miniredis/clock.h"
#include "miniredis/status.h"

/*
 * 키-값 저장소(문자열 타입만, 키·값 모두 바이너리 안전 바이트열).
 *
 * 동시성 설계
 *  - 저장소 전체는 단일 스레드에서만 접근한다. 서버는 이벤트 루프 하나에서 모든
 *    명령을 받은 순서대로 동기적으로 끝까지 실행하므로, 명령 하나는 다른 명령과
 *    끼어들 수 없어 자연스럽게 원자적이다. 락이 없으니 데드락·경합도 없다.
 *  - 한계: 느린 명령(큰 KEYS, 거대한 값 복사 등)은 그동안 다른 모든 클라이언트를
 *    막는다. 또한 한 코어만 사용한다.
 *  - 백그라운드 타이머 스레드가 없으므로 능동 만료는 호출자가 이벤트 루프에서
 *    mr_store_sweep() 을 주기적으로 불러야 진행된다.
 *
 * 만료 설계
 *  - 엔트리마다 절대 만료 시각 expire_at(epoch ms, 0 = 무기한)을 둔다. 상대 TTL
 *    이 아닌 절대 시각이어야 AOF 재생 시점과 무관하게 같은 의미가 된다.
 *  - now >= expire_at 이면 만료로 본다(정확히 그 시각부터 보이지 않음).
 *  - lazy 만료: 키에 접근하는 모든 연산이 먼저 만료를 검사해 지운다.
 *  - active 만료: 별도 만료 인덱스(힙 등)를 두지 않고, 메인 해시 테이블을 커서
 *    (mr_ht_scan)로 조금씩 훑으며 만료된 키를 지운다. 힙은 expire/persist/del
 *    마다 O(log n) 갱신과 위치 추적 메모리가 들고, Redis식 별도 expires 사전은
 *    키당 두 번째 엔트리가 필요하다. 대신 TTL 키 수(volatile)를 세어 0 이면 sweep
 *    을 즉시 끝내므로 TTL 이 없는 워크로드는 비용이 0 이다. 한 바퀴 비용이 전체
 *    키 수에 비례한다는 것이 이 선택의 한계다.
 *
 * 빌린 포인터: get 등이 돌려주는 포인터는 다음 "변경 가능" 호출(이 저장소의
 * const 가 아닌 모든 함수) 전까지만 유효하다.
 */
typedef struct mr_store mr_store;

typedef struct mr_store_opts {
    const mr_clock *clock; /* NULL 이면 mr_clock_system(). 저장소보다 오래 살아야 함 */
    uint8_t hash_seed[16]; /* 해시 플러딩 방지용. 서버는 무작위 값을 넣을 것 */
} mr_store_opts;

typedef enum mr_set_cond {
    MR_SET_ALWAYS = 0,
    MR_SET_NX, /* 키가 없을 때만 */
    MR_SET_XX  /* 키가 있을 때만 */
} mr_set_cond;

typedef enum mr_expire_kind {
    MR_EXPIRE_NONE = 0, /* TTL 제거(일반 SET 의미) */
    MR_EXPIRE_PX,       /* expire 는 지금부터의 상대 ms (> 0) */
    MR_EXPIRE_PXAT      /* expire 는 절대 epoch ms (> 0). 과거면 즉시 만료 */
} mr_expire_kind;

typedef struct mr_set_opts {
    mr_set_cond cond;
    mr_expire_kind expire_kind;
    int64_t expire;
} mr_set_opts;

typedef struct mr_store_stats {
    uint64_t hits;           /* 조회 성공 */
    uint64_t misses;         /* 조회 실패(만료 포함) */
    uint64_t sets;           /* 성공한 쓰기(set/incr) */
    uint64_t deletes;        /* 명시적 삭제(del/invalidate/음수 expire) */
    uint64_t expired_lazy;   /* 접근 시 발견된 만료 */
    uint64_t expired_active; /* sweep/로딩 종료 정리로 지운 만료 */
    size_t keys;             /* 현재 키 수(아직 지우지 못한 만료 키 포함) */
    size_t volatile_keys;    /* TTL 이 있는 키 수 */
    size_t memory_bytes;     /* 추정 사용 메모리(해시 테이블 + 값) */
} mr_store_stats;

/*
 * 순회 콜백. 포인터는 콜백 동안만 유효하고, 콜백 안에서 저장소를 변경하면 안
 * 된다. expire_at_ms 는 0(무기한) 또는 절대 epoch ms. false 를 돌려주면 순회 중단.
 */
typedef bool (*mr_store_visit_fn)(const void *key, size_t klen, const void *val, size_t vlen,
                                  int64_t expire_at_ms, void *ud);

/* 생성. opts NULL 허용. MR_OK / MR_NOMEM / MR_INVALID_ARG(out NULL). */
mr_status mr_store_create(const mr_store_opts *opts, mr_store **out);

/* 해제. NULL 허용. O(n). */
void mr_store_destroy(mr_store *s);

/*
 * SET. opts NULL 이면 무조건 쓰기 + TTL 제거. 값은 복사된다(vlen==0 이면 val NULL 허용).
 * 반환: MR_OK, MR_COND_FAILED(NX/XX 불충족, 변화 없음), MR_INVALID_ARG(PX<=0,
 * PXAT<=0 등), MR_OVERFLOW(now+PX 오버플로), MR_NOMEM(변화 없음).
 * PXAT 가 이미 지난 시각이면 조건 검사 후 키를 지우고 MR_OK(로딩 중엔 보존).
 * 평균 O(klen + vlen).
 */
mr_status mr_store_set(mr_store *s, const void *key, size_t klen, const void *val, size_t vlen,
                       const mr_set_opts *opts);

/* GET. *val, *vlen 에 빌린 포인터. MR_OK / MR_NOT_FOUND / MR_INVALID_ARG. 평균 O(klen). */
mr_status mr_store_get(mr_store *s, const void *key, size_t klen, const void **val,
                       size_t *vlen);

/* DEL. MR_OK / MR_NOT_FOUND. */
mr_status mr_store_del(mr_store *s, const void *key, size_t klen);

/* 살아 있는 키인지(만료 키는 지우고 false). */
bool mr_store_exists(mr_store *s, const void *key, size_t klen);

/*
 * TTL 설정. 0 이하이면 Redis 처럼 키를 즉시 삭제한다(MR_OK).
 * 반환: MR_OK, MR_NOT_FOUND, MR_OVERFLOW(초->ms 또는 now+ttl 오버플로).
 */
mr_status mr_store_expire(mr_store *s, const void *key, size_t klen, int64_t seconds);
mr_status mr_store_pexpire(mr_store *s, const void *key, size_t klen, int64_t ms);
/* 절대 시각 TTL. 과거면 삭제(로딩 중엔 보존). MR_OK / MR_NOT_FOUND / MR_INVALID_ARG(<=0). */
mr_status mr_store_pexpireat(mr_store *s, const void *key, size_t klen, int64_t at_ms);

/*
 * 남은 TTL. 키가 있으면 MR_OK 와 함께 *out 에 남은 ms(무기한이면 -1).
 * mr_store_ttl 은 초 단위(반올림, 무기한 -1). 키가 없으면 MR_NOT_FOUND.
 */
mr_status mr_store_pttl(mr_store *s, const void *key, size_t klen, int64_t *out_ms);
mr_status mr_store_ttl(mr_store *s, const void *key, size_t klen, int64_t *out_sec);

/* TTL 제거. MR_OK(제거함), MR_COND_FAILED(TTL 없던 키), MR_NOT_FOUND. */
mr_status mr_store_persist(mr_store *s, const void *key, size_t klen);

/*
 * 정수 증감. 없는 키는 0 으로 보고 TTL 은 유지한다. 값은 Redis 와 같은 엄격한
 * 10진 표현이어야 한다(앞뒤 공백, '+', 선행 0, "-0" 거부).
 * 반환: MR_OK(*out 새 값; out NULL 허용), MR_NOT_INTEGER, MR_OVERFLOW, MR_NOMEM.
 * 실패 시 변화 없음.
 */
mr_status mr_store_incrby(mr_store *s, const void *key, size_t klen, int64_t delta, int64_t *out);
mr_status mr_store_incr(mr_store *s, const void *key, size_t klen, int64_t *out);
mr_status mr_store_decr(mr_store *s, const void *key, size_t klen, int64_t *out);
mr_status mr_store_decrby(mr_store *s, const void *key, size_t klen, int64_t delta, int64_t *out);

/*
 * glob 패턴(pattern NULL 이면 전체)에 맞는 살아 있는 키를 모두 방문한다.
 * 만료 키는 건너뛴다(지우지 않음: 순회 중 축소로 같은 키가 중복 반환되는 것을
 * 막기 위해). O(n * 패턴 비용). 전체를 막는 명령이므로
 * 큰 저장소에서는 mr_store_scan 을 권장.
 */
mr_status mr_store_keys(mr_store *s, const void *pattern, size_t plen, mr_store_visit_fn fn,
                        void *ud);

/*
 * 커서 기반 부분 순회. cursor 0 으로 시작, *next_cursor 가 0 이면 끝.
 * 대략 count 개(최소 1)의 원소를 살펴본 뒤 돌아온다. 보장은 mr_ht_scan 과 같다
 * (계속 존재한 키는 최소 한 번, 호출 사이에 키가 삭제되어 테이블이 줄면 중복
 * 가능). pattern NULL 이면 전체. fn 이 false 를 돌려주면 *next_cursor 는 0.
 */
mr_status mr_store_scan(mr_store *s, uint64_t cursor, const void *pattern, size_t plen,
                        size_t count, mr_store_visit_fn fn, void *ud, uint64_t *next_cursor);

/*
 * 데이터 무효화: glob 패턴 또는 접두사에 맞는 키를 모두 삭제하고 *deleted 에
 * 지운 살아 있는 키 수를 넣는다(deleted NULL 허용). O(n).
 */
mr_status mr_store_invalidate(mr_store *s, const void *pattern, size_t plen, size_t *deleted);
mr_status mr_store_invalidate_prefix(mr_store *s, const void *prefix, size_t plen,
                                     size_t *deleted);

/* 모든 키 삭제. 실패하지 않는다. O(n). */
void mr_store_flushall(mr_store *s);

/* 키 수(아직 회수되지 않은 만료 키 포함). O(1). */
size_t mr_store_size(const mr_store *s);

/* 통계 스냅샷. O(1). */
void mr_store_stats_get(const mr_store *s, mr_store_stats *out);

/*
 * 능동 만료. 이벤트 루프가 주기적으로(예: 100ms 마다) 호출한다.
 * 내부 커서에서 이어서 최대 약 max_samples 개 원소를 살펴보며 만료 키를 지운다.
 * time_budget_ms > 0 이면 단조 시계로 그 시간을 넘기면 멈춘다(0 = 시간 제한 없음).
 * 반환: 지운 키 수. TTL 키가 없으면 즉시 0.
 */
size_t mr_store_sweep(mr_store *s, size_t max_samples, int64_t time_budget_ms);

/*
 * 로딩 모드(AOF 재생용). 켜져 있는 동안 만료 검사를 하지 않고 과거 시각 TTL 도
 * 그대로 보존한다. 재생 도중 이미 지난 TTL 의 키라도, 원래 시점에는 살아 있다가
 * 이후 PERSIST 된 경우를 올바르게 재현하기 위해서다(Redis 도 로딩 중 만료하지
 * 않음). 끌 때 만료된 키를 모두 정리하고 그 수를 돌려준다.
 */
size_t mr_store_set_loading(mr_store *s, bool loading);

#endif /* MINIREDIS_STORE_H */
