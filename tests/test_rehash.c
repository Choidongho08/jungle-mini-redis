/* 내부 상태도 확인해 전체 재해싱으로 되돌아가는 회귀를 검출한다. */
#include "../src/storage/store.c"

#include <assert.h>
#include <stdio.h>

#define TEXT(value) ((Bytes){(const unsigned char *)(value), sizeof(value) - 1})

typedef struct {
    char text[13][32];
    Bytes keys[13];
} Fixture;

static Store *start_rehash(Fixture *fixture, size_t bucket)
{
    Store *store = store_create();
    assert(store);
    size_t found = 0;
    for (unsigned candidate = 0; found < 13; candidate++) {
        char text[32];
        size_t len = (size_t)snprintf(text, sizeof(text), "key:%u", candidate);
        Bytes key = {(const unsigned char *)text, len};
        if (bucket_index(hash_key(key), INITIAL_BUCKETS) != bucket)
            continue;
        memcpy(fixture->text[found], text, len);
        fixture->keys[found] = (Bytes){(const unsigned char *)fixture->text[found], len};
        found++;
    }
    for (size_t i = 0; i < 13; i++)
        assert(store_set(store, fixture->keys[i], TEXT("old"), i < 12 ? 100 : 0, 0)
               == STORE_OK);
    assert(store->capacity == 16 && store->new_capacity == 32);
    assert(store->rehash_index == 0 && store->count == 13);
    return store;
}

static void check_invariants(const Store *store, const Fixture *fixture)
{
    size_t count = 0, bytes = 0;
    unsigned seen[13] = {0};
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
                for (size_t k = 0; k < 13; k++) {
                    if (entry->key_len == fixture->keys[k].len &&
                        memcmp(entry->data, fixture->keys[k].data, entry->key_len) == 0)
                        assert(++seen[k] == 1);
                }
            }
        }
    }
    assert(count == store->count && bytes == store->entry_bytes);
}

static void test_one_bucket_per_command(void)
{
    Fixture fixture;
    Store *store = start_rehash(&fixture, 4);
    Entry *original = store->buckets[4];
    Bytes value;
    for (size_t step = 1; step <= 16; step++) {
        assert(!store_get(store, TEXT("missing"), 0, &value));
        if (step < 16) {
            assert(store->new_buckets && store->rehash_index == step);
            if (step <= 4)
                assert(store->buckets[4] == original);
            else
                assert(!store->buckets[4]);
        } else {
            assert(!store->new_buckets && store->capacity == 32);
            assert(store->rehash_index == 0 && store->new_capacity == 0);
        }
        check_invariants(store, &fixture);
    }
    for (size_t i = 0; i < 13; i++)
        assert(store_get(store, fixture.keys[i], 0, &value));
    store_destroy(store);
}

static void test_operations_during_rehash(void)
{
    Fixture fixture;
    Store *store = start_rehash(&fixture, 15);
    Bytes value;
    assert(store_get(store, fixture.keys[0], 0, &value));
    assert(store_get(store, fixture.keys[12], 0, &value));
    assert(store_set(store, fixture.keys[0], TEXT("updated"), 0, 0) == STORE_OK);
    assert(store_set(store, fixture.keys[12], TEXT("new"), 0, 0) == STORE_OK);
    assert(store->count == 13);
    assert(store_delete(store, fixture.keys[1], 0));
    assert(store_delete(store, fixture.keys[12], 0));
    assert(store_pttl(store, fixture.keys[2], 10) == 90);
    assert(!store_get(store, fixture.keys[3], 100, &value));
    assert(store_pttl(store, fixture.keys[4], 100) == -2);
    assert(store->rehash_index == 9 && store->count == 9);
    check_invariants(store, &fixture);
    store_sweep(store, 100, 48);
    assert(store->new_buckets && store->count == 1);
    check_invariants(store, &fixture);
    while (store->new_buckets)
        store_sweep(store, 100, 1);
    assert(store_get(store, fixture.keys[0], 100, &value));
    assert(value.len == 7 && memcmp(value.data, "updated", 7) == 0);
    assert(store_pttl(store, fixture.keys[0], 100) == -1);
    for (size_t i = 1; i < 13; i++)
        assert(!store_delete(store, fixture.keys[i], 100));
    check_invariants(store, &fixture);
    store_destroy(store);
}

static void test_sweep_and_destroy_during_rehash(void)
{
    Fixture fixture;
    Store *store = start_rehash(&fixture, 15);
    /* 양쪽 배열의 키를 모두 만료시킨다. SET도 이동 한 단계를 진행한다. */
    assert(store_set(store, fixture.keys[12], TEXT("expires"), 100, 0) == STORE_OK);
    store_sweep(store, 100, 0);
    assert(store->count == 13 && store->rehash_index == 1);
    store_sweep(store, 100, 1);
    assert(store->count == 13 && store->rehash_index == 2);
    assert(store->sweep_index == 1);
    store_sweep(store, 100, 48);
    assert(store->new_buckets && store->count == 0 && store->entry_bytes == 0);
    check_invariants(store, &fixture);
    store_destroy(store);

    /* 양쪽 배열에 살아 있는 체인이 남은 상태에서도 모두 해제해야 한다. */
    store = start_rehash(&fixture, 15);
    store_destroy(store);
}

int main(void)
{
    test_one_bucket_per_command();
    test_operations_during_rehash();
    test_sweep_and_destroy_during_rehash();
    puts("Incremental rehash tests passed");
    return EXIT_SUCCESS;
}
