#include "siphash.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static uint64_t rotl(uint64_t x, unsigned n) {
    return (x << n) | (x >> (64 - n));
}

static uint64_t load64_le(const uint8_t *p) {
    uint64_t word = 0;
    /* Byte assembly avoids alignment and host-endian dependencies. */
    for (unsigned i = 0; i < 8; i++) word |= (uint64_t)p[i] << (8 * i);
    return word;
}

static void rounds(uint64_t v[4], unsigned count) {
    for (unsigned i = 0; i < count; i++) {
        v[0] += v[1]; v[1] = rotl(v[1], 13); v[1] ^= v[0];
        v[0] = rotl(v[0], 32);
        v[2] += v[3]; v[3] = rotl(v[3], 16); v[3] ^= v[2];
        v[0] += v[3]; v[3] = rotl(v[3], 21); v[3] ^= v[0];
        v[2] += v[1]; v[1] = rotl(v[1], 17); v[1] ^= v[2];
        v[2] = rotl(v[2], 32);
    }
}

uint64_t siphash(const uint8_t *in, size_t len, const uint8_t key[16]) {
    uint64_t k0 = load64_le(key), k1 = load64_le(key + 8);
    uint64_t v[4] = {
        UINT64_C(0x736f6d6570736575) ^ k0,
        UINT64_C(0x646f72616e646f6d) ^ k1,
        UINT64_C(0x6c7967656e657261) ^ k0,
        UINT64_C(0x7465646279746573) ^ k1
    };
    size_t pos = 0;
    uint64_t tail = (uint64_t)(len & 255) << 56;
    while (len - pos >= 8) {
        uint64_t m = load64_le(in + pos);
        v[3] ^= m;
        rounds(v, SIPHASH_C_ROUNDS);
        v[0] ^= m;
        pos += 8;
    }
    for (size_t i = 0; i < len - pos; i++)
        tail |= (uint64_t)in[pos + i] << (8 * i);
    v[3] ^= tail;
    rounds(v, SIPHASH_C_ROUNDS);
    v[0] ^= tail;
    v[2] ^= 255;
    rounds(v, SIPHASH_D_ROUNDS);
    return v[0] ^ v[1] ^ v[2] ^ v[3];
}

int siphash_seed_init(uint8_t out[16]) {
    int fd;
    do { fd = open("/dev/urandom", O_RDONLY); } while (fd < 0 && errno == EINTR);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < 16) {
        ssize_t n = read(fd, out + used, 16 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int saved = n == 0 ? EIO : errno;
            close(fd);
            errno = saved;
            return -1;
        }
        used += (size_t)n;
    }
    return close(fd) == 0 ? 0 : -1;
}
