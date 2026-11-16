#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "persistence/persistence.h"
#include "common/time_utils.h"
#include "protocol/resp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LOCK_FILE "persistence.lock"
#define SNAPSHOT_FILE "dump.rdb"
#define SNAPSHOT_TEMP "dump.rdb.tmp"
#define LEGACY_MANIFEST "persistence.manifest"
#define MAX_RECORD (MAX_REQUEST + 128u)

static const char snapshot_magic[] = "MRS2";
static const char legacy_snapshot_magic[] = "MRS1";

struct Persistence {
    int directory;
    int lock;
};

typedef struct {
    int64_t monotonic;
    uint64_t unix_ms;
} ClockSample;

static bool sample_clock(ClockSample *clock)
{
    int64_t monotonic = now_ms();
    struct timespec wall;
    if (clock_gettime(CLOCK_REALTIME, &wall) < 0)
        return false;
    if (wall.tv_sec < 0 || (uint64_t)wall.tv_sec > (INT64_MAX - 999) / 1000) {
        errno = EOVERFLOW;
        return false;
    }
    *clock = (ClockSample){monotonic, (uint64_t)wall.tv_sec * 1000 + wall.tv_nsec / 1000000};
    return true;
}

static void pack(unsigned char *buffer, size_t length, uint64_t number)
{
    for (size_t i = length; i > 0; i--) {
        buffer[i - 1] = (unsigned char)(number & 255);
        number >>= 8;
    }
}

static uint64_t unpack(const unsigned char *buffer, size_t length)
{
    uint64_t number = 0;
    for (size_t i = 0; i < length; i++)
        number = (number << 8) | buffer[i];
    return number;
}

static bool put(FILE *file, const void *data, size_t length)
{
    return length == 0 || fwrite(data, 1, length, file) == length;
}

static bool sync_file(FILE *file)
{
    return fflush(file) == 0 && fsync(fileno(file)) == 0;
}

static bool write_entry(FILE *file, Bytes key, Bytes value, uint64_t expires)
{
    unsigned char header[17];
    if (key.len > MAX_RECORD - 13 || value.len > MAX_RECORD - 13 - key.len) {
        errno = EINVAL;
        return false;
    }
    pack(header, 4, 13 + key.len + value.len);
    header[4] = 'S';
    pack(header + 5, 8, expires);
    pack(header + 13, 4, key.len);
    return put(file, header, sizeof(header)) &&
           put(file, key.data, key.len) && put(file, value.data, value.len);
}

static uint64_t unix_deadline(ClockSample clock, int64_t deadline)
{
    if (!deadline)
        return 0;
    return deadline <= clock.monotonic
        ? clock.unix_ms
        : clock.unix_ms + (uint64_t)(deadline - clock.monotonic);
}

static bool restore_entry(Store *store, const unsigned char *data, size_t len,
                          ClockSample clock)
{
    if (len < 13 || data[0] != 'S')
        return false;
    uint64_t expires = unpack(data + 1, 8);
    size_t key_len = unpack(data + 9, 4);
    if (key_len > len - 13)
        return false;

    int64_t deadline = 0;
    if (expires) {
        if (expires <= clock.unix_ms)
            return true;
        uint64_t remaining = expires - clock.unix_ms;
        uint64_t max = (uint64_t)(INT64_MAX - clock.monotonic);
        deadline = remaining >= max ? INT64_MAX : clock.monotonic + (int64_t)remaining;
    }

    Bytes key = {data + 13, key_len};
    Bytes value = {data + 13 + key_len, len - 13 - key_len};
    return store_set(store, key, value, deadline, clock.monotonic) == STORE_OK;
}

static bool restore_entries(FILE *file, Store *store, ClockSample clock)
{
    unsigned char *data = malloc(MAX_RECORD);
    if (!data)
        return false;
    bool success = false;
    for (;;) {
        unsigned char length[4];
        size_t read = fread(length, 1, sizeof(length), file);
        if (read == 0 && feof(file) && !ferror(file)) {
            success = true;
            break;
        }
        if (read != sizeof(length))
            break;
        size_t size = unpack(length, sizeof(length));
        if (size < 13 || size > MAX_RECORD || fread(data, 1, size, file) != size ||
            !restore_entry(store, data, size, clock))
            break;
    }
    free(data);
    return success;
}

static bool read_magic(FILE *file, const char *magic)
{
    char actual[4];
    return fread(actual, 1, sizeof(actual), file) == sizeof(actual) &&
           memcmp(actual, magic, sizeof(actual)) == 0;
}

static FILE *open_stream(int directory, const char *name, int flags, const char *mode)
{
    int fd = openat(directory, name, flags | O_NOFOLLOW, 0600);
    if (fd < 0)
        return NULL;
    FILE *file = fdopen(fd, mode);
    if (!file) {
        int error = errno;
        close(fd);
        errno = error;
    }
    return file;
}

static const char *legacy_snapshot_name(int directory, bool *required)
{
    FILE *manifest = open_stream(directory, LEGACY_MANIFEST, O_RDONLY, "rb");
    if (!manifest)
        return errno == ENOENT ? "snapshot.rdb" : NULL;

    int slot = fgetc(manifest);
    bool valid = (slot == '0' || slot == '1') &&
                 fgetc(manifest) == '\n' && fgetc(manifest) == EOF && !ferror(manifest);
    fclose(manifest);
    if (!valid) {
        errno = EINVAL;
        return NULL;
    }
    *required = true;
    return slot == '0' ? "snapshot.0.rdb" : "snapshot.1.rdb";
}

static bool restore(Persistence *p, Store *store)
{
    FILE *file = open_stream(p->directory, SNAPSHOT_FILE, O_RDONLY, "rb");
    bool legacy = false;
    if (!file) {
        if (errno != ENOENT)
            return false;
        bool required = false;
        const char *name = legacy_snapshot_name(p->directory, &required);
        if (!name)
            return false;
        file = open_stream(p->directory, name, O_RDONLY, "rb");
        if (!file)
            return !required && errno == ENOENT;
        legacy = true;
    }

    ClockSample clock;
    /* MRS1 stored an eight-byte AOF offset before its snapshot records. */
    unsigned char old_offset[8];
    errno = 0;
    bool ok = sample_clock(&clock) &&
              read_magic(file, legacy ? legacy_snapshot_magic : snapshot_magic) &&
              (!legacy || fread(old_offset, 1, sizeof(old_offset), file) == sizeof(old_offset)) &&
              restore_entries(file, store, clock);
    int error = errno;
    int closed = fclose(file);
    if (!ok)
        errno = error ? error : EINVAL;
    return ok && closed == 0;
}

Persistence *persistence_open(const char *directory, Store *store)
{
    Persistence *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->lock = -1;
    p->directory = open(directory, O_RDONLY | O_DIRECTORY);
    if (p->directory < 0)
        goto fail;
    p->lock = openat(p->directory, LOCK_FILE, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (p->lock < 0 || fcntl(p->lock, F_SETLK, &lock) < 0)
        goto fail;
    if (!restore(p, store))
        goto fail;
    return p;

fail: {
    int error = errno;
    persistence_close(p);
    errno = error;
    return NULL;
}
}

void persistence_close(Persistence *p)
{
    if (!p)
        return;
    if (p->lock >= 0)
        close(p->lock);
    if (p->directory >= 0)
        close(p->directory);
    free(p);
}

typedef struct {
    FILE *file;
    ClockSample clock;
} Snapshot;

static bool snapshot_entry(Bytes key, Bytes value, int64_t deadline, void *context)
{
    Snapshot *snapshot = context;
    return write_entry(snapshot->file, key, value, unix_deadline(snapshot->clock, deadline));
}

bool persistence_save(Persistence *p, const Store *store)
{
    Snapshot snapshot;
    if (!sample_clock(&snapshot.clock))
        return false;
    snapshot.file = open_stream(p->directory, SNAPSHOT_TEMP,
                                O_WRONLY | O_CREAT | O_TRUNC, "wb");
    if (!snapshot.file)
        return false;

    bool ok = put(snapshot.file, snapshot_magic, sizeof(snapshot_magic) - 1) &&
              store_visit(store, snapshot.clock.monotonic, snapshot_entry, &snapshot) &&
              sync_file(snapshot.file);
    int error = errno;
    int closed = fclose(snapshot.file);
    if (!ok)
        errno = error;
    if (!ok || closed < 0 ||
        renameat(p->directory, SNAPSHOT_TEMP, p->directory, SNAPSHOT_FILE) < 0) {
        error = errno;
        unlinkat(p->directory, SNAPSHOT_TEMP, 0);
        errno = error;
        return false;
    }
    return fsync(p->directory) == 0;
}
