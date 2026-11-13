#include "dict.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t total, failures, freed;

static void check(int ok, const char *what) {
    total++;
    if (!ok) {
        failures++;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void counted_free(void *val) {
    freed++;
    free(val);
}

static int *value(int n) {
    int *p = xmalloc(sizeof *p);
    *p = n;
    return p;
}

int main(void) {
    uint8_t seed[16] = {0};
    dict_set_seed(seed);
    dict_t *d = dict_create(counted_free);
    check(dict_size(d) == 0, "empty size");
    check(dict_find(d, "missing", 7) == NULL, "absent find");
    check(dict_delete(d, "missing", 7) == 0, "absent delete");
    char key[] = {'a', '\0', 'b'};
    check(dict_set(d, key, sizeof key, value(11)) == 1, "binary insert");
    key[0] = 'z';
    int *p = dict_find(d, "a\0b", 3);
    check(p && *p == 11, "key bytes copied");
    check(dict_find(d, "a", 1) == NULL, "key length matters");
    check(dict_set(d, "a\0b", 3, value(22)) == 0, "replace return");
    check(freed == 1 && dict_size(d) == 1, "replace frees old value");
    p = dict_find(d, "a\0b", 3);
    check(p && *p == 22, "replacement found");
    check(dict_delete(d, "a\0b", 3) == 1, "delete return");
    check(freed == 2 && dict_size(d) == 0, "delete frees value");
    check(dict_find(d, "a\0b", 3) == NULL, "deleted absent");
    check(dict_delete(d, "a\0b", 3) == 0 && freed == 2, "delete twice");

    /* The fourth insertion reaches load factor one and triggers 4 -> 8. */
    for (int i = 0; i < 5; i++) {
        char k = (char)i;
        check(dict_set(d, &k, 1, value(i)) == 1, "resize insert");
        for (int j = 0; j <= i; j++) {
            char existing = (char)j;
            p = dict_find(d, &existing, 1);
            check(p && *p == j, "find across resize boundary");
        }
    }
    dict_clear(d);
    check(dict_size(d) == 0 && freed == 7, "clear destroys all");
    dict_clear(d);
    check(dict_size(d) == 0 && freed == 7, "clear empty table");

    for (int i = 0; i < 100000; i++) {
        char k[32];
        int len = snprintf(k, sizeof k, "key:%d", i);
        check(dict_set(d, k, (size_t)len, value(i)) == 1, "100k insert");
    }
    check(dict_size(d) == 100000, "100k size");
    for (int i = 0; i < 100000; i++) {
        char k[32];
        int len = snprintf(k, sizeof k, "key:%d", i);
        p = dict_find(d, k, (size_t)len);
        check(p && *p == i, "100k correct value");
    }
    dict_clear(d);
    check(dict_size(d) == 0 && freed == 100007, "100k clear releases values");
    check(dict_find(d, "key:0", 5) == NULL, "clear removes keys");
    check(dict_set(d, "", 0, value(99)) == 1, "empty key after clear");
    p = dict_find(d, "", 0);
    check(p && *p == 99, "table reusable");
    dict_free(d);
    check(freed == 100008, "free destroys remaining value");

    /* A NULL callback leaves values under the caller's ownership. */
    int borrowed = 42;
    d = dict_create(NULL);
    check(dict_set(d, "borrowed", 8, &borrowed) == 1, "borrowed insert");
    check(dict_set(d, "borrowed", 8, &borrowed) == 0, "borrowed replace");
    check(dict_delete(d, "borrowed", 8) == 1, "borrowed delete");
    dict_set(d, "clear", 5, &borrowed);
    dict_clear(d);
    dict_set(d, "free", 4, &borrowed);
    dict_free(d);
    check(borrowed == 42, "NULL callback preserves borrowed value");
    printf("test_dict: %zu/%zu passed\n", total - failures, total);
    return failures ? 1 : 0;
}
