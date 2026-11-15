#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* NUL 종료 문자열 대신 포인터와 길이로 바이트열을 표현한다. */
typedef struct {
    const unsigned char *data;
    size_t len;
} Bytes;

/* 내부 필드는 store.c에만 공개한다. 외부에서는 함수로만 저장소를 조작한다. */
typedef struct Store Store;

typedef enum {
    STORE_OK,
    STORE_FULL,
    STORE_NO_MEMORY
} StoreResult;

Store *store_create(void);
void store_destroy(Store *store);
/* deadline과 now는 같은 단조 시계의 밀리초 값. deadline 0은 TTL 없음.
 * 키·값을 내부 메모리에 복사하므로 호출 뒤 입력 버퍼를 재사용해도 된다. */
StoreResult store_set(Store *store, Bytes key, Bytes value, int64_t deadline, int64_t now);
/* 반환 값은 저장소 내부 메모리를 빌려 보는 뷰다. 다음 변경 전에 복사해야 한다. */
bool store_get(Store *store, Bytes key, int64_t now, Bytes *value);
bool store_delete(Store *store, Bytes key, int64_t now);
/* 남은 TTL(ms), 만료 없음 -1, 키 없음 -2를 반환한다. */
int64_t store_pttl(Store *store, Bytes key, int64_t now);
/* 두 배열을 합쳐 bucket_budget개 이하의 버킷을 만료 검사한다.
 * 양수 예산이면 별도로 기존 버킷 하나를 재해싱한다. 0이면 아무 작업도 하지 않는다. */
void store_sweep(Store *store, int64_t now, size_t bucket_budget);

/* 방문 중에는 Store를 변경하지 않는다. 만료된 Entry는 건너뛴다. */
typedef bool (*StoreVisitor)(Bytes key, Bytes value, int64_t deadline, void *context);
bool store_visit(const Store *store, int64_t now, StoreVisitor visit, void *context);

#endif
