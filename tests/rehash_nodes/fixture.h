/* store.c를 포함한 실험 파일에서 사용한다. 실제 FNV 해시로 충돌 키를 만든다. */
#ifndef REHASH_FIXTURE_H
#define REHASH_FIXTURE_H

#include <assert.h>

static void collision_keys(uint64_t *keys, size_t count, size_t capacity)
{
    size_t found = 0;
    for (uint64_t candidate = 0; found < count; candidate++) {
        Bytes key = {(const unsigned char *)&candidate, sizeof(candidate)};
        if (bucket_index(hash_key(key), capacity) == 0)
            keys[found++] = candidate;
    }
}

/* SET의 검색·확장 비용을 제외하고 동일한 재해싱 직전 상태를 구성한다. */
static Store *rehash_fixture(const uint64_t *keys, size_t count, size_t capacity)
{
    Store *store = store_create();
    assert(store);
    free(store->buckets);
    store->capacity = capacity;
    store->buckets = calloc(capacity, sizeof(*store->buckets));
    assert(store->buckets);
    for (size_t i = 0; i < count; i++) {
        Bytes key = {(const unsigned char *)&keys[i], sizeof(keys[i])};
        Entry *entry = malloc(sizeof(*entry) + key.len + 1);
        assert(entry);
        entry->hash = hash_key(key);
        entry->key_len = key.len;
        entry->value_len = 1;
        entry->deadline = 0;
        memcpy(entry->data, key.data, key.len);
        entry->data[key.len] = 'v';
        size_t bucket = bucket_index(entry->hash, capacity);
        entry->next = store->buckets[bucket];
        store->buckets[bucket] = entry;
        store->count++;
        store->entry_bytes += entry_size(entry);
    }
    assert(grow(store));
    return store;
}

#endif
