#include "dict.h"
#include "siphash.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

typedef struct dict_entry {
    char *key;
    size_t len;
    void *val;
    struct dict_entry *next;
} dict_entry;

struct dict {
    dict_entry **buckets;
    size_t size, used;
    void (*val_free)(void *);
};

static uint8_t hash_seed[16];

void dict_set_seed(const uint8_t seed[16]) {
    memcpy(hash_seed, seed, sizeof hash_seed);
}

static size_t bucket_index(const char *key, size_t len, size_t size) {
    /* Power-of-two capacity permits a mask instead of division. */
    return (size_t)(siphash((const uint8_t *)key, len, hash_seed) & (size - 1));
}

dict_t *dict_create(void (*val_free)(void *)) {
    dict_t *d = xmalloc(sizeof *d);
    d->size = 4;
    d->used = 0;
    d->val_free = val_free;
    d->buckets = xcalloc(d->size, sizeof *d->buckets);
    return d;
}

static dict_entry **find_link(dict_t *d, const char *key, size_t len) {
    dict_entry **link = &d->buckets[bucket_index(key, len, d->size)];
    while (*link) {
        if ((*link)->len == len && (len == 0 || memcmp((*link)->key, key, len) == 0))
            break;
        link = &(*link)->next;
    }
    return link;
}

void *dict_find(dict_t *d, const char *key, size_t len) {
    dict_entry *entry = *find_link(d, key, len);
    return entry ? entry->val : NULL;
}

static void grow(dict_t *d) {
    /* Refuse capacity overflow rather than allocate an undersized table. */
    if (d->size > SIZE_MAX / 2 / sizeof *d->buckets) abort();
    size_t size = d->size * 2;
    dict_entry **buckets = xcalloc(size, sizeof *buckets);
    for (size_t i = 0; i < d->size; i++) {
        dict_entry *entry = d->buckets[i];
        while (entry) {
            dict_entry *next = entry->next;
            size_t idx = bucket_index(entry->key, entry->len, size);
            entry->next = buckets[idx];
            buckets[idx] = entry;
            entry = next;
        }
    }
    free(d->buckets);
    d->buckets = buckets;
    d->size = size;
}

int dict_set(dict_t *d, const char *key, size_t len, void *val) {
    dict_entry *entry = *find_link(d, key, len);
    if (entry) {
        if (d->val_free) d->val_free(entry->val);
        entry->val = val;
        return 0;
    }
    entry = xmalloc(sizeof *entry);
    entry->key = xmalloc(len);
    if (len) memcpy(entry->key, key, len);
    entry->len = len;
    entry->val = val;
    size_t idx = bucket_index(key, len, d->size);
    entry->next = d->buckets[idx];
    d->buckets[idx] = entry;
    d->used++;
    if (d->used >= d->size) grow(d);
    return 1;
}

static void destroy_entry(dict_t *d, dict_entry *entry) {
    if (d->val_free) d->val_free(entry->val);
    free(entry->key);
    free(entry);
}

int dict_delete(dict_t *d, const char *key, size_t len) {
    dict_entry **link = find_link(d, key, len);
    if (!*link) return 0;
    dict_entry *entry = *link;
    *link = entry->next;
    destroy_entry(d, entry);
    d->used--;
    return 1;
}

static void destroy_entries(dict_t *d) {
    for (size_t i = 0; i < d->size; i++) {
        dict_entry *entry = d->buckets[i];
        while (entry) {
            dict_entry *next = entry->next;
            destroy_entry(d, entry);
            entry = next;
        }
    }
    free(d->buckets);
}

void dict_clear(dict_t *d) {
    destroy_entries(d);
    d->size = 4;
    d->used = 0;
    d->buckets = xcalloc(d->size, sizeof *d->buckets);
}

size_t dict_size(const dict_t *d) { return d->used; }

void dict_free(dict_t *d) {
    if (!d) return;
    destroy_entries(d);
    free(d);
}
