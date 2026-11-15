#ifndef STORE_SOURCE
#define STORE_SOURCE "../../src/storage/store.c"
#endif
#include STORE_SOURCE
#include "fixture.h"

#include <stdio.h>

static Bytes key_bytes(const uint64_t *key)
{
    return (Bytes){(const unsigned char *)key, sizeof(*key)};
}

static size_t chain_length(const Entry *entry)
{
    size_t count = 0;
    for (; entry; entry = entry->next)
        count++;
    return count;
}

static void check_invariants(Store *store, const uint64_t *keys, size_t key_count)
{
    size_t count = 0, bytes = 0;
    unsigned seen[129] = {0};
    assert(key_count <= 129);
    for (size_t table = 0; table < 2; table++) {
        Entry **buckets = table ? store->new_buckets : store->buckets;
        size_t capacity = table ? store->new_capacity : store->capacity;
        for (size_t i = 0; i < capacity; i++) {
            if (!table && i < store->rehash_index)
                assert(!buckets[i]);
            for (Entry *entry = buckets[i]; entry; entry = entry->next) {
                assert(bucket_index(entry->hash, capacity) == i);
                count++;
                bytes += entry_size(entry);
                bool known = false;
                for (size_t k = 0; k < key_count; k++) {
                    if (entry->key_len == sizeof(keys[k]) &&
                        memcmp(entry->data, &keys[k], sizeof(keys[k])) == 0) {
                        assert(++seen[k] == 1);
                        known = true;
                    }
                }
                assert(known);
            }
        }
    }
    assert(count == store->count && bytes == store->entry_bytes);
}

int main(void)
{
    uint64_t keys[129];
    collision_keys(keys, 129, 256);
    Store *store = rehash_fixture(keys, 128, 256);
    size_t bytes = store->entry_bytes;
    rehash_step_budget(store, 0);
    assert(chain_length(store->buckets[0]) == 128 && store->rehash_index == 0);
    rehash_step_budget(store, 1);
    assert(chain_length(store->buckets[0]) == 127 && store->rehash_index == 0);
    rehash_step_budget(store, 2);
    assert(chain_length(store->buckets[0]) == 125 && store->rehash_index == 0);
    assert(store->count == 128 && store->entry_bytes == bytes);
    check_invariants(store, keys, 129);

    /* 중간 상태에서 양쪽 표에 나뉜 키를 조회한다. */
    assert(find_link(store, key_bytes(&keys[127]), hash_key(key_bytes(&keys[127])), 0));
    assert(find_link(store, key_bytes(&keys[0]), hash_key(key_bytes(&keys[0])), 0));
    Bytes updated = {(const unsigned char *)"updated", 7};
    assert(store_set(store, key_bytes(&keys[127]), updated, 0, 0) == STORE_OK);
    assert(chain_length(store->buckets[0]) == 125 - REHASH_NODE_BUDGET);
    assert(store_set(store, key_bytes(&keys[0]), updated, 0, 0) == STORE_OK);
    assert(store_delete(store, key_bytes(&keys[126]), 0));
    assert(store_delete(store, key_bytes(&keys[1]), 0));
    Entry **link = find_link(store, key_bytes(&keys[2]), hash_key(key_bytes(&keys[2])), 0);
    assert(link);
    (*link)->deadline = 100;
    assert(store_pttl(store, key_bytes(&keys[2]), 10) == 90);
    Bytes value;
    assert(!store_get(store, key_bytes(&keys[2]), 100, &value));
    assert(store_set(store, key_bytes(&keys[128]), updated, 0, 100) == STORE_OK);
    check_invariants(store, keys, 129);
    assert(store->count == 126);

    size_t index = store->rehash_index;
    size_t remaining = chain_length(store->buckets[index]);
    store_sweep(store, 100, 0);
    assert(store->rehash_index == index);
    assert(chain_length(store->buckets[index]) == remaining);
    store_sweep(store, 100, 1);
    check_invariants(store, keys, 129);
    size_t calls = 0;
    while (store->new_buckets) {
        assert(++calls < 1024);
        rehash_step_budget(store, 1);
        check_invariants(store, keys, 129);
    }
    assert(store->capacity == 512 && store->rehash_index == 0);
    assert(store_get(store, key_bytes(&keys[0]), 100, &value));
    assert(value.len == 7 && memcmp(value.data, "updated", 7) == 0);
    assert(!store_get(store, key_bytes(&keys[126]), 100, &value));
    assert(!store_get(store, key_bytes(&keys[1]), 100, &value));
    assert(!store_get(store, key_bytes(&keys[2]), 100, &value));
    store_destroy(store);

    /* 양쪽 테이블에 체인이 남아 있어도 종료와 만료 정리가 안전해야 한다. */
    store = rehash_fixture(keys, 128, 256);
    rehash_step_budget(store, 1);
    store_destroy(store);
    store = rehash_fixture(keys, 128, 256);
    rehash_step_budget(store, 1);
    for (size_t table = 0; table < 2; table++) {
        Entry **buckets = table ? store->new_buckets : store->buckets;
        size_t capacity = table ? store->new_capacity : store->capacity;
        for (size_t i = 0; i < capacity; i++)
            for (Entry *entry = buckets[i]; entry; entry = entry->next)
                entry->deadline = 100;
    }
    store_sweep(store, 100, 768);
    assert(store->count == 0 && store->entry_bytes == 0);
    while (store->new_buckets)
        rehash_step(store);
    store_destroy(store);
    puts("Node-budget rehash correctness tests passed");
    return 0;
}
