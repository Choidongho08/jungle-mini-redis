#ifndef MINI_REDIS_DICT_H
#define MINI_REDIS_DICT_H

#include <stddef.h>
#include <stdint.h>

typedef struct dict dict_t;

/* Keys are copied; values are owned through the optional destruction callback. */
dict_t *dict_create(void (*val_free)(void *));
/* Return the stored value, or NULL if absent (also ambiguous with a NULL value). */
void *dict_find(dict_t *d, const char *key, size_t len);
/* Return 1 inserted, 0 replaced; replacement destroys the previous value. */
int dict_set(dict_t *d, const char *key, size_t len, void *val);
/* Return 1 deleted, 0 absent; deletion destroys the value. */
int dict_delete(dict_t *d, const char *key, size_t len);
/* Destroy all entries and reset to four buckets, keeping the dict usable. */
void dict_clear(dict_t *d);
/* Number of entries, rather than buckets. */
size_t dict_size(const dict_t *d);
/* Destroy all entries and the table itself. */
void dict_free(dict_t *d);
/* Set once before populating any dict: changing it invalidates existing hashes.
 * A random process-wide secret prevents predictable HashDoS collisions. */
void dict_set_seed(const uint8_t seed[16]);

#endif
