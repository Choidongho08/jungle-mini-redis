#ifndef MINIREDIS_HT_H
#define MINIREDIS_HT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "miniredis/status.h"

/*
 * 바이너리 안전 해시 테이블(체이닝 + 점진적 리해싱).
 *
 * 설계 요약
 *  - 버킷 수는 항상 2의 거듭제곱이라 hash & (cap-1) 로 인덱싱한다.
 *  - 기본 해시는 직접 구현한 SipHash-1-3(128비트 시드). 공격자가 키를 고를 수
 *    있는 서버에서 FNV 같은 비키드 해시는 같은 버킷으로 몰리는 키를 쉽게 만들어
 *    O(n) 체인을 유발(해시 플러딩)한다. 시드를 모르면 충돌 키를 미리 계산할 수
 *    없으므로 서버는 시작 시 무작위 시드를 넣어야 한다.
 *  - 리사이즈 시 두 테이블 t[0](기존)/t[1](새)을 두고 put/get/delete 마다 버킷
 *    1개(빈 버킷은 최대 10개까지 건너뜀)씩만 옮긴다. 수백만 키를 한 번에 옮기는
 *    지연 스파이크를 없애기 위함이다. 리해싱 중 삽입은 t[1] 에만, 조회·삭제는
 *    양쪽 모두에서 한다.
 *  - 확장: 원소 수 >= 버킷 수(로드 팩터 1.0). 축소: 로드 팩터 < 0.1.
 *    확장용 버킷 배열 할당이 실패해도 put 자체는 성공시킨다(체인이 길어질 뿐).
 *
 * 값 소유권: 값은 불투명 포인터이며 opts.val_free 가 있으면 테이블이 소유한다.
 * 덮어쓰기·삭제·clear·destroy 시 기존 값에 val_free 를 호출한다. 키는 put 시
 * 내부로 복사되므로 호출자 버퍼는 반환 즉시 재사용 가능하다.
 *
 * 스레드: 단일 스레드 전용. 동기화하지 않는다.
 */
typedef struct mr_ht mr_ht;

typedef void (*mr_ht_val_free_fn)(void *val, void *ud);
typedef uint64_t (*mr_ht_hash_fn)(const void *key, size_t len, const uint8_t seed[16]);

typedef struct mr_ht_opts {
    size_t initial_capacity;   /* 0 이면 첫 삽입 때 할당. 2의 거듭제곱으로 올림. */
    mr_ht_val_free_fn val_free;  /* NULL 이면 값을 해제하지 않음 */
    void *val_free_ud;
    mr_ht_hash_fn hash;          /* NULL 이면 SipHash-1-3. 테스트에서 충돌 유도용. */
    uint8_t seed[16];
} mr_ht_opts;

/* scan 콜백이 돌려주는 동작. DELETE 는 현재 원소를 지우고 val_free 를 호출한다. */
typedef enum mr_ht_scan_action { MR_HT_SCAN_KEEP = 0, MR_HT_SCAN_DELETE = 1 } mr_ht_scan_action;

/* key/val 는 콜백 동안만 유효. 콜백 안에서 테이블에 put/delete 하면 안 된다. */
typedef mr_ht_scan_action (*mr_ht_scan_fn)(const void *key, size_t klen, void *val, void *ud);

/*
 * 테이블 생성. opts 가 NULL 이면 기본값(모든 필드 0).
 * 반환: MR_OK(*out 설정), MR_NOMEM, MR_INVALID_ARG(out NULL), MR_OVERFLOW(용량 과대).
 */
mr_status mr_ht_create(const mr_ht_opts *opts, mr_ht **out);

/* 모든 원소의 값에 val_free 를 호출하고 해제한다. NULL 허용. O(n). */
void mr_ht_destroy(mr_ht *ht);

/*
 * 키가 없으면 삽입, 있으면 값을 교체한다(기존 값에 val_free 호출).
 * 실패 시 val 의 소유권은 호출자에게 남고 테이블은 변하지 않는다.
 * key 는 klen==0 이면 NULL 허용. 평균 O(1).
 * 반환: MR_OK, MR_NOMEM, MR_INVALID_ARG, MR_OVERFLOW(키 길이로 인한 크기 오버플로).
 * replaced 가 NULL 이 아니면 교체 여부를 기록한다.
 */
mr_status mr_ht_put(mr_ht *ht, const void *key, size_t klen, void *val, bool *replaced);

/*
 * 조회. 찾으면 *val 에 빌린 포인터를 넣고 MR_OK, 없으면 MR_NOT_FOUND.
 * 점진적 리해싱을 한 걸음 진행하므로 const 가 아니다. 평균 O(1).
 */
mr_status mr_ht_get(mr_ht *ht, const void *key, size_t klen, void **val);

/* 키 존재 여부. mr_ht_get 과 같은 비용. */
bool mr_ht_contains(mr_ht *ht, const void *key, size_t klen);

/* 삭제(val_free 호출). MR_OK 또는 MR_NOT_FOUND. 평균 O(1). 축소를 시작할 수 있다. */
mr_status mr_ht_delete(mr_ht *ht, const void *key, size_t klen);

/* 모든 원소 삭제(val_free 호출) 후 버킷 배열까지 해제한다. 실패하지 않는다. O(n). */
void mr_ht_clear(mr_ht *ht);

/* 원소 수. O(1). */
size_t mr_ht_size(const mr_ht *ht);

/* 현재 할당된 버킷 수(리해싱 중이면 두 테이블의 합). O(1). */
size_t mr_ht_capacity(const mr_ht *ht);

/* 리해싱 진행 중인지. 테스트·진단용. O(1). */
bool mr_ht_is_rehashing(const mr_ht *ht);

/*
 * 메모리 사용량 추정(바이트): 구조체 + 버킷 배열 + 엔트리 헤더 + 키 바이트.
 * 값이 가리키는 메모리와 malloc 내부 오버헤드는 포함하지 않는다. O(1).
 */
size_t mr_ht_memory_usage(const mr_ht *ht);

/*
 * 커서 기반 반복(Redis dictScan 의 역비트 커서 알고리즘).
 * cursor 0 으로 시작해 반환값을 다음 호출에 넘기고, 0 이 반환되면 끝이다.
 * 한 호출은 버킷 하나(리해싱 중이면 작은 테이블 버킷 1개 + 대응하는 큰 테이블
 * 버킷들)를 방문한다.
 *
 * 보장: 반복 시작부터 끝까지 계속 존재한 원소는 호출 사이에 리사이즈/리해싱이
 * 일어나도 최소 한 번 반환된다(중복 반환은 가능). 커서의 상위 비트부터 증가
 * 시키므로 테이블이 커지거나 줄어도 이미 훑은 버킷 집합이 새 테이블에서도 접두
 * 구간으로 유지되기 때문이다.
 *
 * 콜백은 MR_HT_SCAN_DELETE 로 현재 원소만 삭제할 수 있다. 스캔 한 걸음 안에서는
 * 리해싱·리사이즈를 하지 않고, 삭제가 있었다면 걸음이 끝난 뒤 축소를 검토한다.
 */
uint64_t mr_ht_scan(mr_ht *ht, uint64_t cursor, mr_ht_scan_fn fn, void *ud);

#endif /* MINIREDIS_HT_H */
