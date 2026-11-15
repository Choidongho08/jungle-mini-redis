/* 키·값 저장 계층: 해시로 버킷을 고르고, 충돌은 연결 리스트로 처리한다.
 * 서버가 단일 스레드에서 호출하므로 이 계층에는 별도의 락이 없다. */
#include "storage/store.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_BUCKETS 16
#define MAX_ENTRIES 16384
#define MAX_ENTRY_BYTES (32u * 1024u * 1024u)

/* Entry는 버킷 자체가 아니라 키·값 한 쌍을 담는 연결 리스트 노드다. */
typedef struct Entry {
    struct Entry *next; /* 같은 버킷에 속한 다음 노드. 마지막이면 NULL. */
    uint64_t hash; /* 재해시와 키 비교에 재사용하는 해시값. */
    size_t key_len;
    size_t value_len;
    int64_t deadline; /* 단조 시계 기준 만료 시각(ms). 0은 만료 없음. */
    /* 가변 길이 배열: [키 key_len 바이트][값 value_len 바이트].
     * 문자열 끝의 NUL 대신 길이로 구분하므로 바이너리도 저장할 수 있다. */
    unsigned char data[];
} Entry;

/* 해시 테이블 전체. buckets[i]는 i번 버킷의 첫 Entry를 가리킨다. */
struct Store {
    Entry **buckets; /* Entry * 원소들로 이루어진 동적 배열. */
    size_t capacity; /* 버킷 개수. 항상 2의 거듭제곱으로 유지한다. */
    size_t count; /* 모든 버킷에 저장된 Entry의 총개수. */
    size_t entry_bytes; /* Entry 할당량 합계. 버킷 배열 메모리는 제외한다. */
    size_t sweep_index; /* 기존 배열과 새 배열을 이어 붙인 순회 위치. */
    Entry **new_buckets; /* 재해싱 중에만 존재하는 목적지 배열. */
    size_t new_capacity;
    size_t rehash_index; /* 이 번호보다 작은 기존 버킷은 모두 비어 있다. */
};

/* FNV-1a: 키의 모든 바이트를 섞어 64비트 해시값을 만든다. */
static uint64_t hash_key(Bytes key)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < key.len; i++) {
        hash ^= key.data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t bucket_index(uint64_t hash, size_t capacity)
{
    /* capacity가 2의 거듭제곱이면 나머지 연산과 같은 결과다. */
    return hash & (capacity - 1);
}

Store *store_create(void)
{
    Store *store = calloc(1, sizeof(*store));
    if (!store)
        return NULL;

    store->capacity = INITIAL_BUCKETS;
    /* 모든 버킷을 빈 리스트(NULL)로 초기화한다. */
    store->buckets = calloc(store->capacity, sizeof(*store->buckets));
    if (!store->buckets) {
        free(store);
        return NULL;
    }
    return store;
}

/* 구조체와 그 뒤에 붙인 키·값 영역을 한 번에 할당한 크기. */
static size_t entry_size(const Entry *entry)
{
    return sizeof(*entry) + entry->key_len + entry->value_len;
}

/* link는 삭제할 노드를 가리키는 포인터 변수의 주소다.
 * 첫 노드이면 &buckets[i], 중간 노드이면 &이전노드->next가 된다. */
static void remove_entry(Store *store, Entry **link)
{
    Entry *entry = *link;
    *link = entry->next; /* 앞쪽 링크가 다음 노드를 가리키도록 우회한다. */
    store->entry_bytes -= entry_size(entry);
    store->count--;
    free(entry);
}

/* 각 버킷의 노드를 모두 해제한 뒤 버킷 배열과 Store를 해제한다. */
void store_destroy(Store *store)
{
    for (size_t i = 0; i < store->capacity; i++) {
        while (store->buckets[i])
            remove_entry(store, &store->buckets[i]);
    }
    for (size_t i = 0; i < store->new_capacity; i++) {
        while (store->new_buckets[i])
            remove_entry(store, &store->new_buckets[i]);
    }
    free(store->buckets);
    free(store->new_buckets);
    free(store);
}

static bool expired(const Entry *entry, int64_t now)
{
    return entry->deadline != 0 && entry->deadline <= now;
}

static Entry **find_link(Store *store, Bytes key, uint64_t hash, int64_t now)
{
    /* 재해싱 중에는 두 배열에 키가 나뉜다. 각 배열의 용량으로 위치를 계산한다.
     * 반환한 링크를 사용하는 동안에는 rehash_step()을 호출하면 안 된다. */
    for (size_t table = 0; table < 2; table++) {
        Entry **buckets = table == 0 ? store->buckets : store->new_buckets;
        size_t capacity = table == 0 ? store->capacity : store->new_capacity;
        if (!buckets)
            break;
        size_t index = bucket_index(hash, capacity);
        if (table == 0 && index < store->rehash_index)
            continue;
        Entry **link = &buckets[index];
        while (*link) {
            Entry *entry = *link;
            if (entry->hash == hash && entry->key_len == key.len &&
                (key.len == 0 || memcmp(entry->data, key.data, key.len) == 0)) {
                if (expired(entry, now)) {
                    remove_entry(store, link);
                    return NULL;
                }
                return link;
            }
            link = &entry->next;
        }
    }
    return NULL;
}

/* 새 배열만 확보한다. Entry 이동은 이후 호출들에 나누어 수행한다. */
static bool grow(Store *store)
{
    size_t capacity = store->capacity * 2;
    Entry **buckets = calloc(capacity, sizeof(*buckets));
    if (!buckets)
        return false;
    store->new_buckets = buckets;
    store->new_capacity = capacity;
    store->rehash_index = 0;
    return true;
}

/* 빈 버킷도 한 단계로 세어 긴 빈 구간을 한꺼번에 순회하지 않는다.
 * 버킷 하나의 체인 전체를 옮기므로 Entry 수나 실행 시간의 제한은 아니다. */
static void rehash_step(Store *store)
{
    if (!store->new_buckets)
        return;
    Entry *entry = store->buckets[store->rehash_index];
    store->buckets[store->rehash_index] = NULL;
    while (entry) {
        Entry *next = entry->next;
        size_t index = bucket_index(entry->hash, store->new_capacity);
        entry->next = store->new_buckets[index];
        store->new_buckets[index] = entry;
        entry = next;
    }
    store->rehash_index++;
    if (store->rehash_index == store->capacity) {
        free(store->buckets);
        store->buckets = store->new_buckets;
        store->capacity = store->new_capacity;
        store->new_buckets = NULL;
        store->new_capacity = 0;
        store->rehash_index = 0;
        store->sweep_index = 0;
    }
}

StoreResult store_set(Store *store, Bytes key, Bytes value, int64_t deadline, int64_t now)
{
    /* 합계를 먼저 더하지 않고 빼서 검사해 크기 계산의 오버플로를 피한다. */
    if (key.len > MAX_ENTRY_BYTES - sizeof(Entry) ||
        value.len > MAX_ENTRY_BYTES - sizeof(Entry) - key.len)
        return STORE_FULL;

    rehash_step(store);
    uint64_t hash = hash_key(key);
    Entry **link = find_link(store, key, hash, now);
    /* 덮어쓰기는 기존 Entry 크기를 제외한 최종 사용량으로 한도를 판단한다. */
    size_t old_size = link ? entry_size(*link) : 0;
    size_t size = sizeof(Entry) + key.len + value.len;
    if ((!link && store->count == MAX_ENTRIES) ||
        size > MAX_ENTRY_BYTES - (store->entry_bytes - old_size))
        return STORE_FULL;

    /* 새 메모리 확보에 실패해도 기존 값은 유지되도록 먼저 할당한다. */
    Entry *entry = malloc(size);
    if (!entry)
        return STORE_NO_MEMORY;

    entry->hash = hash;
    entry->key_len = key.len;
    entry->value_len = value.len;
    entry->deadline = deadline;
    /* 요청 버퍼와 독립적으로 보관하기 위해 키와 값을 저장소 메모리에 복사한다. */
    if (key.len)
        memcpy(entry->data, key.data, key.len);
    if (value.len)
        memcpy(entry->data + key.len, value.data, value.len);

    /* 새 키를 넣을 때 기존 점유율이 75% 이상이면 버킷 수를 두 배로 늘린다. */
    if (!link && !store->new_buckets &&
        store->count >= store->capacity * 3 / 4 && !grow(store)) {
        free(entry);
        return STORE_NO_MEMORY;
    }

    if (link) {
        /* 기존 노드의 위치를 새 노드로 교체한다. 새 deadline으로 TTL도 갱신된다. */
        Entry *old = *link;
        entry->next = old->next;
        *link = entry;
        free(old);
    } else {
        /* 재해싱 중 새 키는 목적지에만 삽입한다. 기존 키는 찾은 자리에서 교체한다. */
        Entry **buckets = store->new_buckets ? store->new_buckets : store->buckets;
        size_t capacity = store->new_buckets ? store->new_capacity : store->capacity;
        size_t index = bucket_index(hash, capacity);
        entry->next = buckets[index];
        buckets[index] = entry;
        store->count++;
    }
    store->entry_bytes = store->entry_bytes - old_size + size;
    return STORE_OK;
}

bool store_get(Store *store, Bytes key, int64_t now, Bytes *value)
{
    rehash_step(store);
    Entry **link = find_link(store, key, hash_key(key), now);
    if (!link)
        return false;
    Entry *entry = *link;
    /* 복사본이 아닌 내부 값의 뷰다. 다음 저장소 변경 전에 사용하거나 복사해야 한다. */
    *value = (Bytes){entry->data + entry->key_len, entry->value_len};
    return true;
}

bool store_delete(Store *store, Bytes key, int64_t now)
{
    rehash_step(store);
    Entry **link = find_link(store, key, hash_key(key), now);
    if (!link)
        return false;
    remove_entry(store, link);
    return true;
}

/* Redis와 같은 반환 규칙: 없는 키 -2, 만료 없는 키 -1, 나머지는 남은 ms. */
int64_t store_pttl(Store *store, Bytes key, int64_t now)
{
    rehash_step(store);
    Entry **link = find_link(store, key, hash_key(key), now);
    if (!link)
        return -2;
    return (*link)->deadline ? (*link)->deadline - now : -1;
}

/* 요청이 오지 않는 키도 회수한다. 한 번에 검사할 버킷 수를 제한하고
 * 다음 호출에서 sweep_index부터 이어간다. 노드 수나 실행 시간의 제한은 아니다. */
void store_sweep(Store *store, int64_t now, size_t bucket_budget)
{
    if (bucket_budget == 0)
        return;
    rehash_step(store); /* 요청이 없어도 주기 정리 호출로 이사를 진행한다. */
    size_t total = store->capacity + store->new_capacity;
    size_t count = bucket_budget < total ? bucket_budget : total;
    for (size_t i = 0; i < count; i++) {
        size_t index = store->sweep_index;
        Entry **link = index < store->capacity
            ? &store->buckets[index]
            : &store->new_buckets[index - store->capacity];
        while (*link) {
            /* 삭제하면 *link가 이미 다음 노드로 바뀌므로 link를 전진하지 않는다. */
            if (expired(*link, now))
                remove_entry(store, link);
            else
                link = &(*link)->next;
        }
        store->sweep_index = (store->sweep_index + 1) % total;
    }
}

bool store_visit(const Store *store, int64_t now, StoreVisitor visit, void *context)
{
    for (size_t table = 0; table < 2; table++) {
        Entry **buckets = table == 0 ? store->buckets : store->new_buckets;
        size_t capacity = table == 0 ? store->capacity : store->new_capacity;
        for (size_t i = 0; i < capacity; i++) {
            for (const Entry *entry = buckets[i]; entry; entry = entry->next) {
                Bytes key = {entry->data, entry->key_len};
                Bytes value = {entry->data + entry->key_len, entry->value_len};
                if (!expired(entry, now) && !visit(key, value, entry->deadline, context))
                    return false;
            }
        }
    }
    return true;
}
