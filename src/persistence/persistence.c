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

#define LOG_FILE "appendonly.aof"
#define SNAPSHOT_FILE "snapshot.rdb"
#define TEMP_FILE "snapshot.tmp"
#define MAX_RECORD (MAX_REQUEST + 128u)

static const char log_magic[] = "MRA1";
static const char snapshot_magic[] = "MRS1";

struct Persistence {
    int directory;
    FILE *log;
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

static bool write_set(FILE *file, Bytes key, Bytes value, uint64_t expires)
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

/* A write already changed RAM. End the process before acknowledging it or
 * allowing another command to observe state that may not be durable. */
static void log_failed(void)
{
    perror("append-only log");
    _Exit(EXIT_FAILURE);
}

void persistence_record_set(Persistence *p, Bytes key, Bytes value,
                            int64_t deadline)
{
    ClockSample clock;
    if (!sample_clock(&clock) ||
        !write_set(p->log, key, value, unix_deadline(clock, deadline)) ||
        !sync_file(p->log))
        log_failed();
}

void persistence_record_delete(Persistence *p, const Bytes *keys, size_t count)
{
    size_t size = 1;
    for (size_t i = 0; i < count; i++)
        size += 4 + keys[i].len;

    unsigned char header[5];
    pack(header, 4, size);
    header[4] = 'D';
    if (!put(p->log, header, sizeof(header)))
        log_failed();
    for (size_t i = 0; i < count; i++) {
        unsigned char length[4];
        pack(length, sizeof(length), keys[i].len);
        if (!put(p->log, length, sizeof(length)) ||
            !put(p->log, keys[i].data, keys[i].len))
            log_failed();
    }
    if (!sync_file(p->log))
        log_failed();
}

static bool replay_record(Store *store, const unsigned char *data, size_t len,
                          ClockSample clock, bool snapshot)
{
    if (data[0] == 'S' && len >= 13) {
        uint64_t expires = unpack(data + 1, 8);
        size_t key_len = unpack(data + 9, 4);
        if (key_len > len - 13)
            return false;
        Bytes key = {data + 13, key_len};
        Bytes value = {data + 13 + key_len, len - 13 - key_len};
        int64_t deadline = 0;
        if (expires) {
            if (expires <= clock.unix_ms) {
                /* An expired overwrite still supersedes the previous value. */
                store_delete(store, key, clock.monotonic);
                return true;
            }
            uint64_t remaining = expires - clock.unix_ms;
            uint64_t max = (uint64_t)(INT64_MAX - clock.monotonic);
            deadline = remaining >= max ? INT64_MAX : clock.monotonic + (int64_t)remaining;
        }
        return store_set(store, key, value, deadline, clock.monotonic) == STORE_OK;
    }
    if (data[0] != 'D' || snapshot || len < 5)
        return false;
    for (size_t pos = 1; pos < len;) {
        if (len - pos < 4)
            return false;
        size_t key_len = unpack(data + pos, 4);
        pos += 4;
        if (key_len > len - pos)
            return false;
        store_delete(store, (Bytes){data + pos, key_len}, clock.monotonic);
        pos += key_len;
    }
    return true;
}

static bool replay(FILE *file, Store *store, ClockSample clock, bool snapshot)
{
    unsigned char *data = malloc(MAX_RECORD);
    if (!data)
        return false;
    bool success = false;
    for (;;) {
        off_t begin = ftello(file);
        if (begin < 0)
            break;
        unsigned char length[4];
        size_t read = fread(length, 1, sizeof(length), file);
        if (read == 0 && feof(file) && !ferror(file)) {
            success = true;
            break;
        }
        if (read == sizeof(length)) {
            size_t size = unpack(length, sizeof(length));
            if (size == 0 || size > MAX_RECORD)
                break;
            if (fread(data, 1, size, file) == size) {
                if (!replay_record(store, data, size, clock, snapshot))
                    break;
                continue;
            }
        }
        /* A crash can leave an incomplete final record. Snapshots are atomic. */
        if (!snapshot && feof(file) && !ferror(file)) {
            success = fseeko(file, begin, SEEK_SET) == 0 &&
                      ftruncate(fileno(file), begin) == 0 && fsync(fileno(file)) == 0;
        }
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

static bool restore(Persistence *p, Store *store, off_t log_end)
{
    ClockSample clock;
    if (!sample_clock(&clock) ||
        fseeko(p->log, 0, SEEK_SET) < 0 || !read_magic(p->log, log_magic))
        return false;

    int fd = openat(p->directory, SNAPSHOT_FILE, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        if (errno != ENOENT)
            return false;
    } else {
        FILE *snapshot = fdopen(fd, "rb");
        if (!snapshot) {
            close(fd);
            return false;
        }
        unsigned char offset[8];
        bool ok = read_magic(snapshot, snapshot_magic) &&
                  fread(offset, 1, sizeof(offset), snapshot) == sizeof(offset);
        uint64_t start = ok ? unpack(offset, sizeof(offset)) : 0;
        ok = ok && start >= 4 && start <= (uint64_t)log_end &&
             replay(snapshot, store, clock, true);
        fclose(snapshot);
        if (!ok || fseeko(p->log, (off_t)start, SEEK_SET) < 0)
            return false;
    }
    return replay(p->log, store, clock, false) && fseeko(p->log, 0, SEEK_END) == 0;
}

Persistence *persistence_open(const char *directory, Store *store)
{
    Persistence *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    p->directory = open(directory, O_RDONLY | O_DIRECTORY);
    if (p->directory < 0)
        goto fail;
    int fd = openat(p->directory, LOG_FILE, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0)
        goto fail;
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    if (fcntl(fd, F_SETLK, &lock) < 0 || !(p->log = fdopen(fd, "r+b"))) {
        int error = errno;
        close(fd);
        errno = error;
        goto fail;
    }

    if (fseeko(p->log, 0, SEEK_END) < 0)
        goto fail;
    off_t end = ftello(p->log);
    if (end < 0)
        goto fail;
    if (end == 0) {
        if (faccessat(p->directory, SNAPSHOT_FILE, F_OK, 0) == 0) {
            errno = EINVAL;
            goto fail;
        }
        if (errno != ENOENT || !put(p->log, log_magic, 4) ||
            !sync_file(p->log) || fsync(p->directory) < 0)
            goto fail;
        end = 4;
    }
    errno = 0;
    if (!restore(p, store, end)) {
        if (!errno || errno == ENOENT)
            errno = EINVAL;
        goto fail;
    }
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
    if (p->log)
        fclose(p->log);
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
    return write_set(snapshot->file, key, value, unix_deadline(snapshot->clock, deadline));
}

bool persistence_save(Persistence *p, const Store *store)
{
    Snapshot snapshot;
    off_t log_offset = ftello(p->log);
    if (log_offset < 0 || !sample_clock(&snapshot.clock))
        return false;
    int fd = openat(p->directory, TEMP_FILE,
                    O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0)
        return false;
    snapshot.file = fdopen(fd, "wb");
    if (!snapshot.file) {
        int error = errno;
        close(fd);
        errno = error;
        return false;
    }

    unsigned char offset[8];
    pack(offset, sizeof(offset), (uint64_t)log_offset);
    bool ok = put(snapshot.file, snapshot_magic, 4) &&
              put(snapshot.file, offset, sizeof(offset)) &&
              store_visit(store, snapshot.clock.monotonic, snapshot_entry, &snapshot) &&
              sync_file(snapshot.file);
    int error = errno;
    if (fclose(snapshot.file) < 0 && ok) {
        error = errno;
        ok = false;
    }
    if (ok) {
        if (renameat(p->directory, TEMP_FILE, p->directory, SNAPSHOT_FILE) == 0)
            return fsync(p->directory) == 0;
        error = errno;
    }
    unlinkat(p->directory, TEMP_FILE, 0);
    errno = error;
    return false;
}
