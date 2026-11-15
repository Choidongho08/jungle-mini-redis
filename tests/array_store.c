/* Alternate Store for the mixed benchmark. Build with SORTED_BUCKETS=0 or 1. */
#include "storage/store.h"

#include <stdlib.h>
#include <string.h>

#define INITIAL_BUCKETS 16
#define MAX_ENTRIES 16384
#define MAX_ENTRY_BYTES (32u * 1024u * 1024u)

typedef struct {
    uint64_t hash;
    size_t key_len;
    size_t value_len;
    int64_t deadline;
    unsigned char data[];
} Entry;

typedef struct {
    Entry **entries;
    size_t count;
    size_t capacity;
} Bucket;

struct Store {
    Bucket *buckets;
    size_t capacity;
    size_t count;
    size_t entry_bytes;
    size_t sweep_index;
};

static uint64_t hash_key(Bytes key)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < key.len; i++) {
        hash ^= key.data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static size_t entry_size(const Entry *entry)
{
    return sizeof(*entry) + entry->key_len + entry->value_len;
}

static bool expired(const Entry *entry, int64_t now)
{
    return entry->deadline != 0 && entry->deadline <= now;
}

static bool reserve(Bucket *bucket)
{
    if (bucket->count < bucket->capacity)
        return true;
    size_t capacity = bucket->capacity ? bucket->capacity * 2 : 1;
    Entry **entries = realloc(bucket->entries, capacity * sizeof(*entries));
    if (!entries)
        return false;
    bucket->entries = entries;
    bucket->capacity = capacity;
    return true;
}

static void free_buckets(Bucket *buckets, size_t capacity)
{
    for (size_t i = 0; i < capacity; i++)
        free(buckets[i].entries);
    free(buckets);
}

Store *store_create(void)
{
    Store *store = calloc(1, sizeof(*store));
    if (!store)
        return NULL;
    store->capacity = INITIAL_BUCKETS;
    store->buckets = calloc(store->capacity, sizeof(*store->buckets));
    if (!store->buckets) {
        free(store);
        return NULL;
    }
    return store;
}

void store_destroy(Store *store)
{
    for (size_t i = 0; i < store->capacity; i++) {
        Bucket *bucket = &store->buckets[i];
        for (size_t j = 0; j < bucket->count; j++)
            free(bucket->entries[j]);
    }
    free_buckets(store->buckets, store->capacity);
    free(store);
}

static void remove_entry(Store *store, Bucket *bucket, size_t position)
{
    Entry *entry = bucket->entries[position];
    store->entry_bytes -= entry_size(entry);
    store->count--;
    bucket->count--;
    if (SORTED_BUCKETS) {
        memmove(bucket->entries + position, bucket->entries + position + 1,
                (bucket->count - position) * sizeof(*bucket->entries));
    } else {
        bucket->entries[position] = bucket->entries[bucket->count];
    }
    free(entry);
}

/* On a miss, position is the insertion point. */
static bool find(Store *store, Bucket *bucket, Bytes key, uint64_t hash,
                 int64_t now, size_t *position)
{
    size_t start = 0;
    if (SORTED_BUCKETS) {
        size_t end = bucket->count;
        while (start < end) {
            size_t mid = start + (end - start) / 2;
            if (bucket->entries[mid]->hash < hash)
                start = mid + 1;
            else
                end = mid;
        }
    }
    for (; start < bucket->count; start++) {
        Entry *entry = bucket->entries[start];
        if (SORTED_BUCKETS && entry->hash > hash)
            break;
        if (entry->hash == hash && entry->key_len == key.len &&
            (key.len == 0 || memcmp(entry->data, key.data, key.len) == 0)) {
            *position = start;
            if (!expired(entry, now))
                return true;
            remove_entry(store, bucket, start);
            if (!SORTED_BUCKETS)
                *position = bucket->count;
            return false;
        }
    }
    *position = start;
    return false;
}

static bool grow(Store *store)
{
    size_t capacity = store->capacity * 2;
    Bucket *buckets = calloc(capacity, sizeof(*buckets));
    if (!buckets)
        return false;
    for (size_t i = 0; i < store->capacity; i++) {
        Bucket *old = &store->buckets[i];
        /* Each destination has one source bucket, preserving sorted order. */
        for (size_t j = 0; j < old->count; j++) {
            Entry *entry = old->entries[j];
            Bucket *bucket = &buckets[entry->hash & (capacity - 1)];
            if (!reserve(bucket)) {
                free_buckets(buckets, capacity);
                return false;
            }
            bucket->entries[bucket->count++] = entry;
        }
    }
    free_buckets(store->buckets, store->capacity);
    store->buckets = buckets;
    store->capacity = capacity;
    store->sweep_index = 0;
    return true;
}

StoreResult store_set(Store *store, Bytes key, Bytes value, int64_t deadline, int64_t now)
{
    if (key.len > MAX_ENTRY_BYTES - sizeof(Entry) ||
        value.len > MAX_ENTRY_BYTES - sizeof(Entry) - key.len)
        return STORE_FULL;

    uint64_t hash = hash_key(key);
    Bucket *bucket = &store->buckets[hash & (store->capacity - 1)];
    size_t position;
    bool found = find(store, bucket, key, hash, now, &position);
    size_t old_size = found ? entry_size(bucket->entries[position]) : 0;
    size_t size = sizeof(Entry) + key.len + value.len;
    if ((!found && store->count == MAX_ENTRIES) ||
        size > MAX_ENTRY_BYTES - (store->entry_bytes - old_size))
        return STORE_FULL;

    Entry *entry = malloc(size);
    if (!entry)
        return STORE_NO_MEMORY;
    *entry = (Entry){hash, key.len, value.len, deadline};
    if (key.len)
        memcpy(entry->data, key.data, key.len);
    if (value.len)
        memcpy(entry->data + key.len, value.data, value.len);

    if (found) {
        free(bucket->entries[position]);
    } else {
        if (store->count >= store->capacity * 3 / 4) {
            if (!grow(store)) {
                free(entry);
                return STORE_NO_MEMORY;
            }
            bucket = &store->buckets[hash & (store->capacity - 1)];
            find(store, bucket, key, hash, now, &position);
        }
        if (!reserve(bucket)) {
            free(entry);
            return STORE_NO_MEMORY;
        }
        if (SORTED_BUCKETS && position < bucket->count)
            memmove(bucket->entries + position + 1, bucket->entries + position,
                    (bucket->count - position) * sizeof(*bucket->entries));
        bucket->count++;
        store->count++;
    }
    bucket->entries[position] = entry;
    store->entry_bytes = store->entry_bytes - old_size + size;
    return STORE_OK;
}

static Entry *get_entry(Store *store, Bytes key, int64_t now)
{
    uint64_t hash = hash_key(key);
    Bucket *bucket = &store->buckets[hash & (store->capacity - 1)];
    size_t position;
    return find(store, bucket, key, hash, now, &position) ? bucket->entries[position] : NULL;
}

bool store_get(Store *store, Bytes key, int64_t now, Bytes *value)
{
    Entry *entry = get_entry(store, key, now);
    if (!entry)
        return false;
    *value = (Bytes){entry->data + entry->key_len, entry->value_len};
    return true;
}

bool store_delete(Store *store, Bytes key, int64_t now)
{
    uint64_t hash = hash_key(key);
    Bucket *bucket = &store->buckets[hash & (store->capacity - 1)];
    size_t position;
    if (!find(store, bucket, key, hash, now, &position))
        return false;
    remove_entry(store, bucket, position);
    return true;
}

int64_t store_pttl(Store *store, Bytes key, int64_t now)
{
    Entry *entry = get_entry(store, key, now);
    if (!entry)
        return -2;
    return entry->deadline ? entry->deadline - now : -1;
}

void store_sweep(Store *store, int64_t now, size_t bucket_budget)
{
    size_t count = bucket_budget < store->capacity ? bucket_budget : store->capacity;
    for (size_t i = 0; i < count; i++) {
        Bucket *bucket = &store->buckets[store->sweep_index];
        size_t j = 0;
        while (j < bucket->count) {
            if (expired(bucket->entries[j], now))
                remove_entry(store, bucket, j);
            else
                j++;
        }
        store->sweep_index = (store->sweep_index + 1) & (store->capacity - 1);
    }
}

bool store_visit(const Store *store, int64_t now, StoreVisitor visit, void *context)
{
    for (size_t i = 0; i < store->capacity; i++) {
        const Bucket *bucket = &store->buckets[i];
        for (size_t j = 0; j < bucket->count; j++) {
            const Entry *entry = bucket->entries[j];
            Bytes key = {entry->data, entry->key_len};
            Bytes value = {entry->data + entry->key_len, entry->value_len};
            if (!expired(entry, now) && !visit(key, value, entry->deadline, context))
                return false;
        }
    }
    return true;
}
