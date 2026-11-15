#include "storage/store.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEXT(value) ((Bytes){(const unsigned char *)(value), sizeof(value) - 1})

static void test_operations(void)
{
    Store *store = store_create();
    assert(store);
    Bytes value;
    Bytes key = TEXT("k\0x");
    assert(!store_get(store, key, 10, &value));
    assert(store_set(store, key, TEXT("v\0\r\n"), 100, 10) == STORE_OK);
    assert(store_get(store, key, 99, &value));
    assert(value.len == 4 && memcmp(value.data, "v\0\r\n", 4) == 0);
    assert(store_pttl(store, key, 99) == 1);
    assert(!store_get(store, key, 100, &value));
    assert(store_pttl(store, key, 100) == -2);
    assert(!store_delete(store, key, 100));

    assert(store_set(store, key, TEXT("old"), 200, 100) == STORE_OK);
    assert(store_set(store, key, TEXT(""), 0, 110) == STORE_OK);
    assert(store_pttl(store, key, 210) == -1);
    assert(store_get(store, key, 210, &value) && value.len == 0);
    assert(store_delete(store, key, 210));
    assert(!store_delete(store, key, 210));
    store_destroy(store);
}

static void test_collision_and_growth(void)
{
    Store *store = store_create();
    assert(store);
    /* 'a' and 'q' have the same low four FNV-1a bits. */
    assert(store_set(store, TEXT("a"), TEXT("one"), 0, 0) == STORE_OK);
    assert(store_set(store, TEXT("q"), TEXT("two"), 0, 0) == STORE_OK);
    assert(store_delete(store, TEXT("a"), 0));
    Bytes value;
    assert(store_get(store, TEXT("q"), 0, &value));
    assert(value.len == 3 && memcmp(value.data, "two", 3) == 0);

    char key[32];
    for (int i = 0; i < 1024; i++) {
        size_t len = (size_t)snprintf(key, sizeof(key), "key:%d", i);
        Bytes bytes = {(const unsigned char *)key, len};
        assert(store_set(store, bytes, bytes, 0, 0) == STORE_OK);
    }
    for (int i = 0; i < 1024; i++) {
        size_t len = (size_t)snprintf(key, sizeof(key), "key:%d", i);
        assert(store_get(store, (Bytes){(const unsigned char *)key, len}, 0, &value));
        assert(value.len == len && memcmp(value.data, key, len) == 0);
    }
    store_destroy(store);
}

static void test_sweep_reclaims_capacity(void)
{
    Store *store = store_create();
    assert(store);
    size_t len = 12u * 1024u * 1024u;
    unsigned char *data = calloc(len, 1);
    assert(data);
    Bytes large = {data, len};
    assert(store_set(store, TEXT("expired"), large, 20, 10) == STORE_OK);
    assert(store_set(store, TEXT("kept"), large, 0, 10) == STORE_OK);
    assert(store_set(store, TEXT("new"), large, 0, 30) == STORE_FULL);
    assert(store_set(store, TEXT("kept"), TEXT("small"), 0, 30) == STORE_OK);
    assert(store_set(store, TEXT("new"), large, 0, 30) == STORE_OK);
    assert(store_set(store, TEXT("another"), large, 0, 30) == STORE_FULL);
    store_sweep(store, 30, 16);
    assert(store_set(store, TEXT("another"), large, 0, 30) == STORE_OK);
    free(data);
    store_destroy(store);
}

int main(void)
{
    test_operations();
    test_collision_and_growth();
    test_sweep_reclaims_capacity();
    puts("Store unit tests passed");
    return EXIT_SUCCESS;
}
