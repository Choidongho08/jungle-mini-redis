#ifndef MINIREDIS_AOF_H
#define MINIREDIS_AOF_H

#include <stddef.h>
#include <stdint.h>

#include "miniredis/clock.h"
#include "miniredis/status.h"
#include "miniredis/store.h"

/*
 * Append-only 파일 영속화.
 *
 * 형식: 레코드 하나 = RESP 배열 하나(*N\r\n$len\r\n<bytes>\r\n...). 길이 기반이라
 * NUL·개행을 포함한 값도 안전하고, 후속 서버의 요청 형식과 같아 디버깅이 쉽다.
 *
 * 기록되는 정규형(재생기가 이해하는 명령의 전부):
 *   SET k v              SET k v PXAT <epoch-ms>
 *   DEL k                PEXPIREAT k <epoch-ms>     PERSIST k
 *   FLUSHALL             INVALIDATE <glob>          INVALIDATEPREFIX <prefix>
 * 상대 TTL 은 절대 시각으로, INCR 은 결과값 SET(기존 만료 시각 유지)으로 기록해
 * 재생 시각과 무관하게 결과가 같도록 한다. 호출자(서버)는 "성공한 변경"만 기록해야
 * 한다.
 *
 * 쓰기: 레코드를 메모리에서 완성한 뒤 write 루프(부분 쓰기·EINTR 재시도)로 쓴다.
 * 중간에 실패하면 파일을 쓰기 전 길이로 ftruncate 해 반쪽 레코드가 남지 않게
 * 한다(남으면 다음 레코드부터 전부 "중간 손상"이 된다). 되돌리기조차 실패하면
 * 이후 모든 쓰기를 MR_IO_ERROR 로 거부한다.
 *
 * 단일 프로세스·단일 스레드 전제이므로 파일 락은 쓰지 않는다.
 */
typedef struct mr_aof mr_aof;

typedef enum mr_aof_fsync {
    MR_AOF_FSYNC_ALWAYS,   /* 매 레코드 fsync: 가장 안전, 가장 느림 */
    MR_AOF_FSYNC_EVERYSEC, /* mr_aof_tick() 이 1초 이상 지났을 때 fsync. 최대 ~1초 유실 */
    MR_AOF_FSYNC_NO        /* OS 에 맡김 */
} mr_aof_fsync;

typedef struct mr_aof_opts {
    const char *path;      /* 필수. 없으면 생성 */
    mr_aof_fsync fsync;
    const mr_clock *clock; /* NULL 이면 시스템 시계. EVERYSEC 주기·rewrite 만료 판단용 */
} mr_aof_opts;

typedef struct mr_aof_load_info {
    uint64_t records;         /* 적용한 레코드 수 */
    uint64_t truncated_bytes; /* 끝에서 잘라낸 불완전 레코드 바이트 수 */
    size_t expired_dropped;   /* 재생 후 만료되어 버린 키 수 */
} mr_aof_load_info;

/*
 * 파일을 열고(없으면 생성) 내용을 store 에 재생한 뒤 추가 쓰기 준비를 한다.
 * 재생은 store 의 로딩 모드에서 하며 끝나면 만료된 키를 정리한다.
 * 파일 끝의 불완전 레코드(쓰다 죽은 흔적)는 잘라내고 성공한다. 완결된 레코드가
 * 문법·명령·인자 면에서 잘못되었으면 중간 손상으로 보고 MR_CORRUPT.
 * 실패하면 store 는 부분적으로 채워져 있을 수 있으므로 호출자가 버려야 한다.
 * info NULL 허용. 반환: MR_OK, MR_INVALID_ARG, MR_IO_ERROR, MR_CORRUPT, MR_NOMEM.
 * O(파일 크기).
 */
mr_status mr_aof_open(const mr_aof_opts *opts, mr_store *store, mr_aof **out,
                      mr_aof_load_info *info);

/* 정책상 필요한 fsync 를 하고 닫는다. NULL 이면 MR_OK. 실패해도 자원은 해제된다. */
mr_status mr_aof_close(mr_aof *aof);

/* 정규형 레코드 기록. 반환: MR_OK, MR_IO_ERROR, MR_NOMEM, MR_INVALID_ARG. */
mr_status mr_aof_log_set(mr_aof *aof, const void *key, size_t klen, const void *val, size_t vlen,
                         int64_t expire_at_ms);
mr_status mr_aof_log_del(mr_aof *aof, const void *key, size_t klen);
mr_status mr_aof_log_pexpireat(mr_aof *aof, const void *key, size_t klen, int64_t at_ms);
mr_status mr_aof_log_persist(mr_aof *aof, const void *key, size_t klen);
mr_status mr_aof_log_flushall(mr_aof *aof);
mr_status mr_aof_log_invalidate(mr_aof *aof, const void *pattern, size_t plen);
mr_status mr_aof_log_invalidate_prefix(mr_aof *aof, const void *prefix, size_t plen);

/*
 * 이벤트 루프가 주기적으로 호출. EVERYSEC 이면 마지막 fsync 후 1초가 지났고
 * 기록된 데이터가 있을 때 fsync 한다. 다른 정책에서는 아무 일도 하지 않는다.
 */
mr_status mr_aof_tick(mr_aof *aof);

/* 정책과 무관하게 지금 fsync. */
mr_status mr_aof_sync(mr_aof *aof);

/*
 * 압축(rewrite): store 의 현재 살아 있는 키만 임시 파일에 SET 레코드로 쓰고
 * fsync -> rename 으로 원자 교체 -> 디렉터리 fsync(rename 자체의 영속화) 후
 * 새 파일로 이어 쓴다. 실패하면 임시 파일을 지우고 기존 파일을 그대로 쓴다.
 * 반환: MR_OK, MR_IO_ERROR, MR_NOMEM. O(n).
 */
mr_status mr_aof_rewrite(mr_aof *aof, mr_store *store);

/* 현재 파일 크기(바이트). O(1). */
uint64_t mr_aof_size(const mr_aof *aof);

#endif /* MINIREDIS_AOF_H */
