#ifndef MINI_REDIS_SIPHASH_H
#define MINI_REDIS_SIPHASH_H

#include <stddef.h>
#include <stdint.h>

#ifndef SIPHASH_C_ROUNDS
#define SIPHASH_C_ROUNDS 1
#endif
#ifndef SIPHASH_D_ROUNDS
#define SIPHASH_D_ROUNDS 2
#endif

/* Keyed hash; byte input does not require alignment. */
uint64_t siphash(const uint8_t *in, size_t len, const uint8_t key[16]);
/* Read a secret HashDoS-resistant seed; return 0 on success, -1 on failure. */
int siphash_seed_init(uint8_t out[16]);

#endif
