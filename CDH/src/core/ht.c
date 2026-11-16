#include "miniredis/ht.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define HT_MIN_CAP ((size_t)4)
#define HT_NOT_REHASHING SIZE_MAX
/* 리해싱 한 걸음에서 건너뛸 수 있는 빈 버킷 수(버킷당). 희소한 테이블에서
 * 한 연산이 빈 버킷을 수천 개 훑어 지연이 튀는 것을 막는다(Redis 와 동일 값). */
#define HT_EMPTY_VISITS_PER_STEP 10

typedef struct ht_entry {
    struct ht_entry *next;
    void *val;
    uint64_t hash; /* 리해싱 때 키를 다시 해시하지 않고 비교도 빠르게 거른다 */
    size_t klen;
    unsigned char key[];
} ht_entry;

typedef struct ht_table {
    ht_entry **buckets;
    size_t cap; /* 0 또는 2의 거듭제곱 */
    size_t used;
} ht_table;

struct mr_ht {
    ht_table t[2];
    size_t rehash_idx;  /* 다음에 옮길 t[0] 버킷, HT_NOT_REHASHING 이면 비활성 */
    size_t entry_bytes; /* 엔트리 할당 크기 합(메모리 추정용) */
    size_t initial_cap;
    mr_ht_val_free_fn val_free;
    void *val_free_ud;
    mr_ht_hash_fn hash;
    uint8_t seed[16];
};

/* ---------- SipHash-1-3 ---------- */

static uint64_t rotl64(uint64_t x, unsigned b) { return (x << b) | (x >> (64U - b)); }

static uint64_t load_le64(const unsigned char *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8U * i);
    }
    return v;
}

static void sipround(uint64_t v[4]) {
    v[0] += v[1];
    v[1] = rotl64(v[1], 13);
    v[1] ^= v[0];
    v[0] = rotl64(v[0], 32);
    v[2] += v[3];
    v[3] = rotl64(v[3], 16);
    v[3] ^= v[2];
    v[0] += v[3];
    v[3] = rotl64(v[3], 21);
    v[3] ^= v[0];
    v[2] += v[1];
    v[1] = rotl64(v[1], 17);
    v[1] ^= v[2];
    v[2] = rotl64(v[2], 32);
}

/* 압축 1라운드/종료 3라운드 변형. 해시 테이블 용도로 충분한 강도와 속도(Redis 채택). */
static uint64_t siphash13(const void *data, size_t len, const uint8_t seed[16]) {
    const unsigned char *in = data;
    uint64_t k0 = load_le64(seed);
    uint64_t k1 = load_le64(seed + 8);
    uint64_t v[4] = {0x736f6d6570736575ULL ^ k0, 0x646f72616e646f6dULL ^ k1,
                     0x6c7967656e657261ULL ^ k0, 0x7465646279746573ULL ^ k1};
    size_t blocks = len / 8;

    for (size_t i = 0; i < blocks; i++) {
        uint64_t m = load_le64(in + 8 * i);
        v[3] ^= m;
        sipround(v);
        v[0] ^= m;
    }
    uint64_t b = (uint64_t)len << 56;
    for (size_t i = 0; i < (len & 7U); i++) {
        b |= (uint64_t)in[blocks * 8 + i] << (8U * i);
    }
    v[3] ^= b;
    sipround(v);
    v[0] ^= b;
    v[2] ^= 0xff;
    sipround(v);
    sipround(v);
    sipround(v);
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}

/* ---------- 내부 도우미 ---------- */

static bool is_rehashing(const mr_ht *ht) { return ht->rehash_idx != HT_NOT_REHASHING; }

/* n 이상인 최소 2의 거듭제곱(최소 HT_MIN_CAP). 버킷 배열 바이트 수가 넘치면 false. */
static bool cap_for(size_t n, size_t *out) {
    size_t cap = HT_MIN_CAP;
    while (cap < n) {
        if (cap > SIZE_MAX / 2) {
            return false;
        }
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(ht_entry *)) {
        return false;
    }
    *out = cap;
    return true;
}

static bool key_eq(const ht_entry *e, uint64_t h, const void *key, size_t klen) {
    return e->hash == h && e->klen == klen && (klen == 0 || memcmp(e->key, key, klen) == 0);
}

static void free_entry(mr_ht *ht, ht_entry *e) {
    if (ht->val_free != NULL) {
        ht->val_free(e->val, ht->val_free_ud);
    }
    ht->entry_bytes -= sizeof(ht_entry) + e->klen;
    free(e);
}

/* 새 버킷 배열을 할당해 리해싱을 시작한다. 빈 테이블이면 바로 교체한다. */
static mr_status start_resize(mr_ht *ht, size_t new_cap) {
    assert(!is_rehashing(ht));
    if (new_cap == ht->t[0].cap) {
        return MR_OK;
    }
    ht_entry **buckets = calloc(new_cap, sizeof(*buckets));
    if (buckets == NULL) {
        return MR_NOMEM;
    }
    if (ht->t[0].used == 0) {
        free(ht->t[0].buckets);
        ht->t[0].buckets = buckets;
        ht->t[0].cap = new_cap;
        return MR_OK;
    }
    ht->t[1].buckets = buckets;
    ht->t[1].cap = new_cap;
    ht->t[1].used = 0;
    ht->rehash_idx = 0;
    return MR_OK;
}

static void maybe_shrink(mr_ht *ht);

static void finish_rehash(mr_ht *ht) {
    free(ht->t[0].buckets);
    ht->t[0] = ht->t[1];
    ht->t[1] = (ht_table){NULL, 0, 0};
    ht->rehash_idx = HT_NOT_REHASHING;
}

/* 최대 n 개 버킷을 t[0] -> t[1] 로 옮긴다. */
static void rehash_step(mr_ht *ht, size_t n) {
    if (!is_rehashing(ht)) {
        return;
    }
    size_t empty_budget = n * HT_EMPTY_VISITS_PER_STEP;
    ht_table *from = &ht->t[0];
    ht_table *to = &ht->t[1];
    while (n > 0 && from->used > 0) {
        assert(ht->rehash_idx < from->cap);
        ht_entry *e = from->buckets[ht->rehash_idx];
        if (e == NULL) {
            ht->rehash_idx++;
            if (--empty_budget == 0) {
                return;
            }
            continue;
        }
        while (e != NULL) {
            ht_entry *next = e->next;
            size_t idx = (size_t)(e->hash & (to->cap - 1));
            e->next = to->buckets[idx];
            to->buckets[idx] = e;
            from->used--;
            to->used++;
            e = next;
        }
        from->buckets[ht->rehash_idx++] = NULL;
        n--;
    }
    if (from->used == 0) {
        finish_rehash(ht);
        /* 리해싱 중에는 축소를 시작할 수 없으므로 그동안 많이 지워졌다면 이어서 줄인다. */
        maybe_shrink(ht);
    }
}

/* 확장은 최선 노력: 실패해도 기존 테이블에 계속 체이닝하면 된다.
 * 단 버킷이 하나도 없으면 삽입이 불가능하므로 그 경우만 실패를 돌려준다. */
static mr_status maybe_expand(mr_ht *ht) {
    if (is_rehashing(ht)) {
        return MR_OK;
    }
    size_t cap;
    if (ht->t[0].cap == 0) {
        if (!cap_for(ht->initial_cap, &cap)) {
            return MR_OVERFLOW;
        }
        return start_resize(ht, cap);
    }
    if (ht->t[0].used >= ht->t[0].cap && ht->t[0].used <= SIZE_MAX / 2 &&
        cap_for(ht->t[0].used * 2, &cap)) {
        (void)start_resize(ht, cap);
    }
    return MR_OK;
}

static void maybe_shrink(mr_ht *ht) {
    if (is_rehashing(ht) || ht->t[0].cap <= HT_MIN_CAP) {
        return;
    }
    size_t cap;
    /* 줄인 뒤 로드 팩터가 0.5 이하가 되도록 해 곧바로 다시 확장되는 진동을 피한다. */
    if (ht->t[0].used < ht->t[0].cap / 10 && cap_for(ht->t[0].used * 2, &cap)) {
        (void)start_resize(ht, cap);
    }
}

/* 키가 있는 링크(이전 노드의 next 또는 버킷 슬롯)를 찾는다. */
static ht_entry **find_link(mr_ht *ht, const void *key, size_t klen, uint64_t h, int *which) {
    for (int i = 0; i < 2; i++) {
        ht_table *t = &ht->t[i];
        if (t->cap == 0) {
            break;
        }
        ht_entry **link = &t->buckets[h & (t->cap - 1)];
        for (; *link != NULL; link = &(*link)->next) {
            if (key_eq(*link, h, key, klen)) {
                *which = i;
                return link;
            }
        }
        if (!is_rehashing(ht)) {
            break;
        }
    }
    return NULL;
}

/* ---------- 공개 API ---------- */

mr_status mr_ht_create(const mr_ht_opts *opts, mr_ht **out) {
    static const mr_ht_opts defaults = {0};
    if (out == NULL) {
        return MR_INVALID_ARG;
    }
    *out = NULL;
    if (opts == NULL) {
        opts = &defaults;
    }
    mr_ht *ht = calloc(1, sizeof(*ht));
    if (ht == NULL) {
        return MR_NOMEM;
    }
    ht->rehash_idx = HT_NOT_REHASHING;
    ht->initial_cap = opts->initial_capacity;
    ht->val_free = opts->val_free;
    ht->val_free_ud = opts->val_free_ud;
    ht->hash = opts->hash != NULL ? opts->hash : siphash13;
    memcpy(ht->seed, opts->seed, sizeof(ht->seed));

    if (opts->initial_capacity > 0) {
        size_t cap;
        if (!cap_for(opts->initial_capacity, &cap)) {
            free(ht);
            return MR_OVERFLOW;
        }
        mr_status st = start_resize(ht, cap);
        if (st != MR_OK) {
            free(ht);
            return st;
        }
    }
    *out = ht;
    return MR_OK;
}

void mr_ht_clear(mr_ht *ht) {
    for (int i = 0; i < 2; i++) {
        ht_table *t = &ht->t[i];
        for (size_t b = 0; b < t->cap; b++) {
            ht_entry *e = t->buckets[b];
            while (e != NULL) {
                ht_entry *next = e->next;
                free_entry(ht, e);
                e = next;
            }
        }
        free(t->buckets);
        *t = (ht_table){NULL, 0, 0};
    }
    ht->rehash_idx = HT_NOT_REHASHING;
}

void mr_ht_destroy(mr_ht *ht) {
    if (ht == NULL) {
        return;
    }
    mr_ht_clear(ht);
    free(ht);
}

mr_status mr_ht_put(mr_ht *ht, const void *key, size_t klen, void *val, bool *replaced) {
    if (ht == NULL || (key == NULL && klen > 0)) {
        return MR_INVALID_ARG;
    }
    if (replaced != NULL) {
        *replaced = false;
    }
    rehash_step(ht, 1);

    uint64_t h = ht->hash(key, klen, ht->seed);
    int which;
    ht_entry **link = find_link(ht, key, klen, h, &which);
    if (link != NULL) {
        void *old = (*link)->val;
        (*link)->val = val;
        if (old != val && ht->val_free != NULL) {
            ht->val_free(old, ht->val_free_ud);
        }
        if (replaced != NULL) {
            *replaced = true;
        }
        return MR_OK;
    }

    if (klen > SIZE_MAX - sizeof(ht_entry)) {
        return MR_OVERFLOW;
    }
    mr_status st = maybe_expand(ht);
    if (st != MR_OK) {
        return st;
    }
    ht_entry *e = malloc(sizeof(ht_entry) + klen);
    if (e == NULL) {
        return MR_NOMEM;
    }
    e->val = val;
    e->hash = h;
    e->klen = klen;
    if (klen > 0) {
        memcpy(e->key, key, klen);
    }
    /* 리해싱 중에는 새 테이블에만 넣어야 옮겨진 버킷에 다시 원소가 생기지 않는다. */
    ht_table *t = is_rehashing(ht) ? &ht->t[1] : &ht->t[0];
    size_t idx = (size_t)(h & (t->cap - 1));
    e->next = t->buckets[idx];
    t->buckets[idx] = e;
    t->used++;
    ht->entry_bytes += sizeof(ht_entry) + klen;
    return MR_OK;
}

mr_status mr_ht_get(mr_ht *ht, const void *key, size_t klen, void **val) {
    if (ht == NULL || (key == NULL && klen > 0)) {
        return MR_INVALID_ARG;
    }
    rehash_step(ht, 1);
    int which;
    ht_entry **link = find_link(ht, key, klen, ht->hash(key, klen, ht->seed), &which);
    if (link == NULL) {
        return MR_NOT_FOUND;
    }
    if (val != NULL) {
        *val = (*link)->val;
    }
    return MR_OK;
}

bool mr_ht_contains(mr_ht *ht, const void *key, size_t klen) {
    return mr_ht_get(ht, key, klen, NULL) == MR_OK;
}

mr_status mr_ht_delete(mr_ht *ht, const void *key, size_t klen) {
    if (ht == NULL || (key == NULL && klen > 0)) {
        return MR_INVALID_ARG;
    }
    rehash_step(ht, 1);
    int which;
    ht_entry **link = find_link(ht, key, klen, ht->hash(key, klen, ht->seed), &which);
    if (link == NULL) {
        return MR_NOT_FOUND;
    }
    ht_entry *e = *link;
    *link = e->next;
    ht->t[which].used--;
    free_entry(ht, e);
    maybe_shrink(ht);
    return MR_OK;
}

size_t mr_ht_size(const mr_ht *ht) { return ht->t[0].used + ht->t[1].used; }

size_t mr_ht_capacity(const mr_ht *ht) { return ht->t[0].cap + ht->t[1].cap; }

bool mr_ht_is_rehashing(const mr_ht *ht) { return is_rehashing(ht); }

size_t mr_ht_memory_usage(const mr_ht *ht) {
    return sizeof(*ht) + (ht->t[0].cap + ht->t[1].cap) * sizeof(ht_entry *) + ht->entry_bytes;
}

/* ---------- scan ---------- */

static uint64_t rev64(uint64_t v) {
    v = ((v >> 1) & 0x5555555555555555ULL) | ((v & 0x5555555555555555ULL) << 1);
    v = ((v >> 2) & 0x3333333333333333ULL) | ((v & 0x3333333333333333ULL) << 2);
    v = ((v >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((v & 0x0F0F0F0F0F0F0F0FULL) << 4);
    v = ((v >> 8) & 0x00FF00FF00FF00FFULL) | ((v & 0x00FF00FF00FF00FFULL) << 8);
    v = ((v >> 16) & 0x0000FFFF0000FFFFULL) | ((v & 0x0000FFFF0000FFFFULL) << 16);
    return (v >> 32) | (v << 32);
}

/* 버킷 하나를 방문한다. 삭제 수를 돌려준다. */
static size_t scan_bucket(mr_ht *ht, ht_table *t, size_t idx, mr_ht_scan_fn fn, void *ud) {
    size_t deleted = 0;
    ht_entry **link = &t->buckets[idx];
    while (*link != NULL) {
        ht_entry *e = *link;
        if (fn(e->key, e->klen, e->val, ud) == MR_HT_SCAN_DELETE) {
            *link = e->next;
            t->used--;
            free_entry(ht, e);
            deleted++;
        } else {
            link = &e->next;
        }
    }
    return deleted;
}

uint64_t mr_ht_scan(mr_ht *ht, uint64_t cursor, mr_ht_scan_fn fn, void *ud) {
    if (ht == NULL || fn == NULL || mr_ht_size(ht) == 0) {
        return 0;
    }
    uint64_t v = cursor;
    uint64_t m0;
    size_t deleted = 0;

    if (!is_rehashing(ht)) {
        ht_table *t = &ht->t[0];
        m0 = (uint64_t)t->cap - 1;
        deleted += scan_bucket(ht, t, (size_t)(v & m0), fn, ud);
    } else {
        ht_table *small = &ht->t[0];
        ht_table *large = &ht->t[1];
        if (small->cap > large->cap) {
            small = &ht->t[1];
            large = &ht->t[0];
        }
        m0 = (uint64_t)small->cap - 1;
        uint64_t m1 = (uint64_t)large->cap - 1;
        deleted += scan_bucket(ht, small, (size_t)(v & m0), fn, ud);
        /* 작은 테이블 버킷 i 에 대응하는 큰 테이블 버킷(하위 비트가 i 인 것)을 모두 방문 */
        do {
            deleted += scan_bucket(ht, large, (size_t)(v & m1), fn, ud);
            v = (((v | m0) + 1) & ~m0) | (v & m0);
        } while (v & (m0 ^ m1));
    }
    /* 마스크 밖 비트를 1로 채운 뒤 역비트 증가: 상위 비트부터 올라가는 커서 */
    v |= ~m0;
    v = rev64(v);
    v++;
    v = rev64(v);

    if (deleted > 0) {
        maybe_shrink(ht);
    }
    return v;
}
