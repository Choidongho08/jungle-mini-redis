/* Share the actual Entry layout, hash function, and Store lookup in this test. */
#include "../src/storage/store.c"

#include <stdio.h>
#include <time.h>

enum { ENTRY_COUNT = 12000, QUERY_COUNT = 100000, ROUNDS = 7, WRITE_BATCH = 240 };

typedef struct {
    size_t *offsets;
    size_t *lengths;
    Entry **entries;
    size_t capacity;
} BucketIndex;

typedef struct {
    const char *name;
    void *context;
    bool (*get)(void *context, Bytes key, Bytes *value);
} Lookup;

static void require(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "%s\n", message);
        exit(EXIT_FAILURE);
    }
}

/* Force longer chains for the stress cases while keeping the same keys and hash. */
static void regroup(Store *store, size_t capacity)
{
    /* 측정용 인덱스는 한 배열을 직접 읽으므로 재해싱을 먼저 완료한다. */
    while (store->new_buckets)
        rehash_step(store);
    if (store->capacity == capacity)
        return;
    Entry **buckets = calloc(capacity, sizeof(*buckets));
    require(buckets != NULL, "cannot allocate stress-test buckets");
    for (size_t i = 0; i < store->capacity; i++) {
        Entry *entry = store->buckets[i];
        while (entry) {
            Entry *next = entry->next;
            size_t index = bucket_index(entry->hash, capacity);
            entry->next = buckets[index];
            buckets[index] = entry;
            entry = next;
        }
    }
    free(store->buckets);
    store->buckets = buckets;
    store->capacity = capacity;
}

static int compare_hash(const void *left, const void *right)
{
    uint64_t a = (*(Entry *const *)left)->hash;
    uint64_t b = (*(Entry *const *)right)->hash;
    return (a > b) - (a < b);
}

static BucketIndex make_index(const Store *store, bool sorted, const Bytes *extra)
{
    BucketIndex index = {
        .offsets = malloc((store->capacity + 1) * sizeof(size_t)),
        .lengths = calloc(store->capacity, sizeof(size_t)),
        .entries = malloc(2 * store->count * sizeof(Entry *)),
        .capacity = store->capacity,
    };
    require(index.offsets && index.lengths && index.entries, "cannot allocate array index");

    /* Reserve space for every candidate key so timing isolates bucket operations. */
    for (size_t i = 0; i < ENTRY_COUNT; i++)
        index.lengths[bucket_index(hash_key(extra[i]), index.capacity)]++;

    size_t end = 0;
    for (size_t i = 0; i < index.capacity; i++) {
        size_t start = end;
        size_t spare = index.lengths[i];
        index.offsets[i] = start;
        for (Entry *entry = store->buckets[i]; entry; entry = entry->next)
            index.entries[end++] = entry;
        if (sorted && end - start > 1)
            qsort(index.entries + start, end - start, sizeof(*index.entries), compare_hash);
        index.lengths[i] = end - start;
        end += spare;
    }
    index.offsets[index.capacity] = end;
    return index;
}

static bool list_get(void *context, Bytes key, Bytes *value)
{
    return store_get(context, key, 0, value);
}

/* On a miss, position is where a new entry belongs. */
static bool array_find(const BucketIndex *index, Bytes key, uint64_t hash,
                       size_t bucket, bool sorted, size_t *position)
{
    size_t start = index->offsets[bucket];
    size_t end = start + index->lengths[bucket];

    if (sorted) {
        size_t upper = end;
        while (start < upper) {
            size_t mid = start + (upper - start) / 2;
            if (index->entries[mid]->hash < hash)
                start = mid + 1;
            else
                upper = mid;
        }
    }

    for (; start < end; start++) {
        const Entry *entry = index->entries[start];
        if (sorted && entry->hash > hash)
            break;
        if (entry->hash == hash && entry->key_len == key.len &&
            (key.len == 0 || memcmp(entry->data, key.data, key.len) == 0)) {
            *position = start;
            return true;
        }
    }
    *position = start;
    return false;
}

static bool array_get(void *context, Bytes key, Bytes *value, bool sorted)
{
    const BucketIndex *index = context;
    uint64_t hash = hash_key(key);
    size_t position;
    if (!array_find(index, key, hash, bucket_index(hash, index->capacity), sorted, &position))
        return false;
    const Entry *entry = index->entries[position];
    if (expired(entry, 0))
        return false;
    *value = (Bytes){entry->data + entry->key_len, entry->value_len};
    return true;
}

static bool linear_get(void *context, Bytes key, Bytes *value)
{
    return array_get(context, key, value, false);
}

static bool binary_get(void *context, Bytes key, Bytes *value)
{
    return array_get(context, key, value, true);
}

static void check_lookup(const Lookup *lookup, Bytes key, bool expected)
{
    Bytes value;
    bool found = lookup->get(lookup->context, key, &value);
    require(found == expected, lookup->name);
    if (found)
        require(value.len == key.len && memcmp(value.data, key.data, key.len) == 0,
                lookup->name);
}

static uint64_t nanoseconds(void)
{
    struct timespec time;
    require(clock_gettime(CLOCK_MONOTONIC, &time) == 0, "clock_gettime failed");
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static double time_lookups(const Lookup *lookup, const Bytes *queries, size_t expected_hits)
{
    size_t hits = 0;
    uint64_t start = nanoseconds();
    for (size_t i = 0; i < QUERY_COUNT; i++) {
        Bytes value;
        hits += lookup->get(lookup->context, queries[i], &value);
    }
    uint64_t elapsed = nanoseconds() - start;
    require(hits == expected_hits, lookup->name);
    return (double)elapsed / QUERY_COUNT;
}

static int compare_samples(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

/* Timed operations include hashing, finding the key, and changing the bucket. */
static bool mutate_list(Store *store, Entry *entry, bool insert)
{
    Bytes key = {entry->data, entry->key_len};
    uint64_t hash = hash_key(key);
    Entry **link = find_link(store, key, hash, 0);
    if (insert && !link) {
        Entry **head = &store->buckets[bucket_index(hash, store->capacity)];
        entry->next = *head;
        *head = entry;
        return true;
    }
    if (!insert && link && *link == entry) {
        *link = entry->next;
        return true;
    }
    return false;
}

static bool mutate_array(BucketIndex *index, Entry *entry, bool insert, bool sorted)
{
    Bytes key = {entry->data, entry->key_len};
    uint64_t hash = hash_key(key);
    size_t bucket = bucket_index(hash, index->capacity);
    size_t end = index->offsets[bucket] + index->lengths[bucket];
    size_t position;
    bool found = array_find(index, key, hash, bucket, sorted, &position);
    if (insert && !found && end < index->offsets[bucket + 1]) {
        if (sorted && position < end)
            memmove(index->entries + position + 1, index->entries + position,
                    (end - position) * sizeof(*index->entries));
        index->entries[position] = entry;
        index->lengths[bucket]++;
        return true;
    }
    if (!insert && found && index->entries[position] == entry) {
        if (!sorted)
            index->entries[position] = index->entries[end - 1];
        else if (position + 1 < end)
            memmove(index->entries + position, index->entries + position + 1,
                    (end - position - 1) * sizeof(*index->entries));
        index->lengths[bucket]--;
        return true;
    }
    return false;
}

static uint64_t time_writes(const Lookup *lookup, size_t method, Entry **batch, bool insert)
{
    size_t changed = 0;
    uint64_t start = nanoseconds();
    for (size_t i = 0; i < WRITE_BATCH; i++) {
        if (method == 0)
            changed += mutate_list(lookup->context, batch[i], insert);
        else
            changed += mutate_array(lookup->context, batch[i], insert, method == 2);
    }
    uint64_t elapsed = nanoseconds() - start;
    require(changed == WRITE_BATCH, lookup->name);
    return elapsed;
}

static size_t existing_key_index(size_t offset)
{
    /* 7919 and 12000 are coprime, so this visits each existing key once. */
    return (offset * 7919) % ENTRY_COUNT;
}

static void benchmark_writes(const Lookup *lookups, Store *store, const Bytes *keys,
                             bool check_only)
{
    Entry *batch[WRITE_BATCH];
    Entry *existing[WRITE_BATCH];
    double samples[2][3][ROUNDS];
    size_t rounds = check_only ? 1 : ROUNDS;
    size_t batches = check_only ? 1 : ENTRY_COUNT / WRITE_BATCH;
    for (size_t round = 0; round < rounds; round++) {
        uint64_t elapsed[2][3] = {{0}};
        for (size_t b = 0; b < batches; b++) {
            for (size_t i = 0; i < WRITE_BATCH; i++) {
                size_t offset = b * WRITE_BATCH + i;
                Bytes original = keys[existing_key_index(offset)];
                Entry **link = find_link(store, original, hash_key(original), 0);
                require(link != NULL, "missing existing write fixture");
                existing[i] = *link;
                Bytes key = keys[ENTRY_COUNT + offset];
                batch[i] = malloc(sizeof(Entry) + 2 * key.len);
                require(batch[i] != NULL, "cannot allocate write fixture");
                *batch[i] = (Entry){.hash = hash_key(key), .key_len = key.len,
                                    .value_len = key.len};
                memcpy(batch[i]->data, key.data, key.len);
                memcpy(batch[i]->data + key.len, key.data, key.len);
            }
            for (size_t position = 0; position < 3; position++) {
                size_t method = (round + position) % 3;
                const Lookup *lookup = &lookups[method];
                elapsed[0][method] += time_writes(lookup, method, batch, true);
                if (check_only)
                    for (size_t i = 0; i < WRITE_BATCH; i++)
                        check_lookup(lookup, keys[ENTRY_COUNT + b * WRITE_BATCH + i], true);
                /* Delete established keys, which need not be near the bucket head. */
                elapsed[1][method] += time_writes(lookup, method, existing, false);
                if (check_only)
                    for (size_t i = 0; i < WRITE_BATCH; i++)
                        check_lookup(lookup, keys[existing_key_index(b * WRITE_BATCH + i)], false);
                time_writes(lookup, method, batch, false);
                time_writes(lookup, method, existing, true);
            }
            for (size_t i = 0; i < WRITE_BATCH; i++)
                free(batch[i]);
        }
        for (size_t op = 0; op < 2; op++)
            for (size_t i = 0; i < 3; i++)
                samples[op][i][round] = (double)elapsed[op][i] / (batches * WRITE_BATCH);
    }
    if (check_only)
        return;
    for (size_t op = 0; op < 2; op++) {
        printf("%-6s", op == 0 ? "insert" : "delete");
        for (size_t i = 0; i < 3; i++) {
            qsort(samples[op][i], ROUNDS, sizeof(double), compare_samples);
            printf("  %-13s %6.1f ns", lookups[i].name, samples[op][i][ROUNDS / 2]);
        }
        putchar('\n');
    }
}

static void benchmark(const Lookup *lookups, size_t count, const Bytes *keys, bool hit)
{
    Bytes *queries = malloc(QUERY_COUNT * sizeof(*queries));
    require(queries != NULL, "cannot allocate queries");
    uint32_t random = 12345;
    for (size_t i = 0; i < QUERY_COUNT; i++) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        queries[i] = keys[random % ENTRY_COUNT];
    }

    size_t expected_hits = hit ? QUERY_COUNT : 0;
    double samples[3][ROUNDS];
    for (size_t i = 0; i < count; i++)
        time_lookups(&lookups[i], queries, expected_hits);
    for (size_t round = 0; round < ROUNDS; round++) {
        for (size_t position = 0; position < count; position++) {
            size_t i = (round + position) % count;
            samples[i][round] = time_lookups(&lookups[i], queries, expected_hits);
        }
    }
    printf("%-4s", hit ? "hit" : "miss");
    for (size_t i = 0; i < count; i++) {
        qsort(samples[i], ROUNDS, sizeof(double), compare_samples);
        printf("  %-13s %6.1f ns", lookups[i].name, samples[i][ROUNDS / 2]);
    }
    putchar('\n');
    free(queries);
}

int main(int argc, char **argv)
{
    bool check_only = argc == 2 && strcmp(argv[1], "--check") == 0;
    require(argc == 1 || check_only, "usage: bench_lookup [--check]");

    char (*key_text)[16] = malloc(2 * ENTRY_COUNT * sizeof(*key_text));
    Bytes *keys = malloc(2 * ENTRY_COUNT * sizeof(*keys));
    require(key_text && keys, "cannot allocate keys");
    for (size_t i = 0; i < 2 * ENTRY_COUNT; i++) {
        unsigned id = (uint32_t)i * UINT32_C(2654435761);
        size_t len = (size_t)snprintf(key_text[i], sizeof(*key_text), "key:%08x", id);
        keys[i] = (Bytes){(const unsigned char *)key_text[i], len};
    }

    const size_t capacities[] = {16384, 2048, 256};
    for (size_t c = 0; c < sizeof(capacities) / sizeof(capacities[0]); c++) {
        Store *store = store_create();
        require(store != NULL, "cannot create store");
        for (size_t i = 0; i < ENTRY_COUNT; i++)
            require(store_set(store, keys[i], keys[i], 0, 0) == STORE_OK, "SET failed");
        regroup(store, capacities[c]);

        BucketIndex linear = make_index(store, false, keys + ENTRY_COUNT);
        BucketIndex binary = make_index(store, true, keys + ENTRY_COUNT);
        const Lookup lookups[] = {
            {"list", store, list_get},
            {"linear-array", &linear, linear_get},
            {"sorted-array", &binary, binary_get},
        };
        size_t count = sizeof(lookups) / sizeof(lookups[0]);
        for (size_t i = 0; i < count; i++) {
            for (size_t k = 0; k < 2 * ENTRY_COUNT; k++)
                check_lookup(&lookups[i], keys[k], k < ENTRY_COUNT);
        }
        if (!check_only) {
            printf("\n%d entries, %zu buckets, load %.2f\n",
                   ENTRY_COUNT, store->capacity, (double)ENTRY_COUNT / store->capacity);
            benchmark(lookups, count, keys, true);
            benchmark(lookups, count, keys + ENTRY_COUNT, false);
        }
        benchmark_writes(lookups, store, keys, check_only);
        for (size_t i = 0; i < count; i++)
            for (size_t k = 0; k < ENTRY_COUNT; k++)
                check_lookup(&lookups[i], keys[k], true);
        free(binary.entries);
        free(binary.lengths);
        free(binary.offsets);
        free(linear.entries);
        free(linear.lengths);
        free(linear.offsets);
        store_destroy(store);
    }
    puts("Lookup, insert, and delete correctness checks passed");
    free(keys);
    free(key_text);
    return EXIT_SUCCESS;
}
