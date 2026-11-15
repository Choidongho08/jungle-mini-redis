#ifndef STORE_SOURCE
#define STORE_SOURCE "../../src/storage/store.c"
#endif
#include STORE_SOURCE
#include "fixture.h"

#include <stdio.h>
#include <time.h>

#define CAPACITY 16384
#define ENTRIES 12288
#define TRIALS 51

static uint64_t ns(void)
{
    struct timespec time;
    assert(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

int main(void)
{
    uint64_t *collisions = malloc(ENTRIES * sizeof(*collisions));
    uint64_t *uniform = malloc(ENTRIES * sizeof(*uniform));
    assert(collisions && uniform);
    collision_keys(collisions, ENTRIES, CAPACITY);
    for (size_t i = 0; i < ENTRIES; i++)
        uniform[i] = i;
    puts("{\"uniform\":[");
    for (int kind = 0; kind < 2; kind++) {
        const uint64_t *keys = kind ? collisions : uniform;
        for (int trial = 0; trial < TRIALS; trial++) {
            Store *store = rehash_fixture(keys, ENTRIES, CAPACITY);
            uint64_t longest = 0, first_nonempty = 0;
            bool saw_nonempty = false;
            size_t traced_calls = 0;
            while (store->new_buckets) {
                bool nonempty = store->buckets[store->rehash_index] != NULL;
                uint64_t begin = ns();
                rehash_step(store);
                uint64_t elapsed = ns() - begin;
                if (elapsed > longest)
                    longest = elapsed;
                if (nonempty && !saw_nonempty) {
                    first_nonempty = elapsed;
                    saw_nonempty = true;
                }
                traced_calls++;
            }
            assert(store->count == ENTRIES);
            store_destroy(store);

            /* 전체 완료 시간은 단계마다 시계를 읽는 비용 없이 별도 측정한다. */
            store = rehash_fixture(keys, ENTRIES, CAPACITY);
            size_t calls = 0;
            uint64_t begin = ns();
            while (store->new_buckets) {
                rehash_step(store);
                calls++;
            }
            uint64_t total = ns() - begin;
            assert(calls == traced_calls && store->count == ENTRIES);
            printf("%s{\"first_nonempty_us\":%.3f,\"max_step_us\":%.3f,"
                   "\"completion_us\":%.3f,\"calls\":%zu}", trial ? ",\n" : "",
                   first_nonempty / 1000.0, longest / 1000.0, total / 1000.0, calls);
            store_destroy(store);
        }
        puts(kind ? "]}" : "],\"collision\":[");
    }
    free(collisions);
    free(uniform);
    return 0;
}
