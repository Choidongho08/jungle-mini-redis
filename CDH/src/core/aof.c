#include "miniredis/aof.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AOF_MAX_ARGC 8
/* RESP 길이 한도(Redis proto-max-bulk-len 기본값과 같은 512MB). 손상된 길이 필드로
 * 터무니없는 메모리를 잡는 것을 막는다. */
#define AOF_MAX_BULK ((size_t)512 * 1024 * 1024)
#define AOF_MAX_LEN_DIGITS 18
#define AOF_READ_CHUNK ((size_t)64 * 1024)
#define AOF_REWRITE_FLUSH ((size_t)64 * 1024)
#define AOF_EVERYSEC_MS 1000
#define AOF_TMP_SUFFIX ".rewrite.tmp"

typedef struct byte_buf {
    unsigned char *data;
    size_t len;
    size_t cap;
} byte_buf;

struct mr_aof {
    int fd;
    char *path;
    mr_aof_fsync policy;
    const mr_clock *clock;
    uint64_t size;
    bool dirty;  /* fsync 되지 않은 쓰기가 있음 */
    bool broken; /* 반쪽 레코드를 되돌리지 못함: 더 쓰면 파일이 손상된다 */
    int64_t last_fsync_ms;
    byte_buf buf;
};

/* ---------- 버퍼 ---------- */

static mr_status buf_reserve(byte_buf *b, size_t extra) {
    if (extra > SIZE_MAX - b->len) {
        return MR_OVERFLOW;
    }
    size_t need = b->len + extra;
    if (need <= b->cap) {
        return MR_OK;
    }
    size_t cap = b->cap > 0 ? b->cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    unsigned char *p = realloc(b->data, cap);
    if (p == NULL) {
        return MR_NOMEM;
    }
    b->data = p;
    b->cap = cap;
    return MR_OK;
}

static mr_status buf_append(byte_buf *b, const void *p, size_t n) {
    mr_status st = buf_reserve(b, n);
    if (st == MR_OK && n > 0) {
        memcpy(b->data + b->len, p, n);
        b->len += n;
    }
    return st;
}

/* "*N\r\n" 또는 "$N\r\n" */
static mr_status buf_append_header(byte_buf *b, char kind, size_t n) {
    char tmp[32];
    int w = snprintf(tmp, sizeof(tmp), "%c%zu\r\n", kind, n);
    if (w < 0 || (size_t)w >= sizeof(tmp)) {
        return MR_OVERFLOW;
    }
    return buf_append(b, tmp, (size_t)w);
}

static mr_status encode_cmd(byte_buf *b, size_t argc, const void *const *argv,
                            const size_t *lens) {
    mr_status st = buf_append_header(b, '*', argc);
    for (size_t i = 0; st == MR_OK && i < argc; i++) {
        st = buf_append_header(b, '$', lens[i]);
        if (st == MR_OK) {
            st = buf_append(b, argv[i], lens[i]);
        }
        if (st == MR_OK) {
            st = buf_append(b, "\r\n", 2);
        }
    }
    return st;
}

/* ---------- 저수준 I/O ---------- */

/* write 는 신호(EINTR)나 디스크 사정으로 일부만 쓸 수 있으므로 끝까지 반복한다. */
static mr_status write_all(int fd, const unsigned char *p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            return MR_IO_ERROR;
        }
        if (w == 0) {
            return MR_IO_ERROR;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return MR_OK;
}

static int fsync_retry(int fd) {
    int rc;
    do {
        rc = fsync(fd);
    } while (rc != 0 && errno == EINTR);
    return rc;
}

/* rename/생성의 영속화는 파일이 아니라 디렉터리 엔트리에 대한 fsync 가 필요하다. */
static mr_status fsync_parent_dir(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t dlen = slash == NULL ? 1 : (slash == path ? 1 : (size_t)(slash - path));
    char *dir = malloc(dlen + 1);
    if (dir == NULL) {
        return MR_NOMEM;
    }
    if (slash == NULL) {
        dir[0] = '.';
    } else {
        memcpy(dir, path, dlen);
    }
    dir[dlen] = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(dir);
    if (fd < 0) {
        return MR_IO_ERROR;
    }
    /* 디렉터리 fsync 를 지원하지 않는 파일시스템(EINVAL)은 실패로 보지 않는다. */
    int rc = fsync_retry(fd);
    bool ok = rc == 0 || errno == EINVAL;
    close(fd);
    return ok ? MR_OK : MR_IO_ERROR;
}

static mr_status do_fsync(mr_aof *aof) {
    if (fsync_retry(aof->fd) != 0) {
        return MR_IO_ERROR;
    }
    aof->dirty = false;
    aof->last_fsync_ms = mr_clock_mono_ms(aof->clock);
    return MR_OK;
}

/* ---------- 기록 ---------- */

static mr_status append_record(mr_aof *aof, size_t argc, const void *const *argv,
                               const size_t *lens) {
    if (aof->broken) {
        return MR_IO_ERROR;
    }
    aof->buf.len = 0;
    mr_status st = encode_cmd(&aof->buf, argc, argv, lens);
    if (st != MR_OK) {
        return st;
    }
    if (aof->buf.len > UINT64_MAX - aof->size) {
        return MR_OVERFLOW;
    }
    st = write_all(aof->fd, aof->buf.data, aof->buf.len);
    if (st != MR_OK) {
        /* 반쪽 레코드를 남기면 이후 레코드가 모두 중간 손상이 되므로 되돌린다. */
        if (ftruncate(aof->fd, (off_t)aof->size) != 0) {
            aof->broken = true;
        }
        return st;
    }
    aof->size += aof->buf.len;
    aof->dirty = true;
    if (aof->policy == MR_AOF_FSYNC_ALWAYS && do_fsync(aof) != MR_OK) {
        /* fsync 실패 후 페이지 캐시 상태는 신뢰할 수 없다: 더 쓰지 않는다. */
        aof->broken = true;
        return MR_IO_ERROR;
    }
    return MR_OK;
}

static bool key_ok(const void *p, size_t n) { return p != NULL || n == 0; }

static int fmt_i64(char *dst, size_t cap, int64_t v) { return snprintf(dst, cap, "%" PRId64, v); }

mr_status mr_aof_log_set(mr_aof *aof, const void *key, size_t klen, const void *val, size_t vlen,
                         int64_t expire_at_ms) {
    if (aof == NULL || !key_ok(key, klen) || !key_ok(val, vlen) || expire_at_ms < 0) {
        return MR_INVALID_ARG;
    }
    char ms[24];
    int n = fmt_i64(ms, sizeof(ms), expire_at_ms);
    const void *argv[5] = {"SET", key, val, "PXAT", ms};
    size_t lens[5] = {3, klen, vlen, 4, (size_t)n};
    return append_record(aof, expire_at_ms == 0 ? 3 : 5, argv, lens);
}

static mr_status log_key_cmd(mr_aof *aof, const char *cmd, const void *key, size_t klen) {
    if (aof == NULL || !key_ok(key, klen)) {
        return MR_INVALID_ARG;
    }
    const void *argv[2] = {cmd, key};
    size_t lens[2] = {strlen(cmd), klen};
    return append_record(aof, 2, argv, lens);
}

mr_status mr_aof_log_del(mr_aof *aof, const void *key, size_t klen) {
    return log_key_cmd(aof, "DEL", key, klen);
}

mr_status mr_aof_log_persist(mr_aof *aof, const void *key, size_t klen) {
    return log_key_cmd(aof, "PERSIST", key, klen);
}

mr_status mr_aof_log_invalidate(mr_aof *aof, const void *pattern, size_t plen) {
    return log_key_cmd(aof, "INVALIDATE", pattern, plen);
}

mr_status mr_aof_log_invalidate_prefix(mr_aof *aof, const void *prefix, size_t plen) {
    return log_key_cmd(aof, "INVALIDATEPREFIX", prefix, plen);
}

mr_status mr_aof_log_pexpireat(mr_aof *aof, const void *key, size_t klen, int64_t at_ms) {
    if (aof == NULL || !key_ok(key, klen) || at_ms <= 0) {
        return MR_INVALID_ARG;
    }
    char ms[24];
    int n = fmt_i64(ms, sizeof(ms), at_ms);
    const void *argv[3] = {"PEXPIREAT", key, ms};
    size_t lens[3] = {9, klen, (size_t)n};
    return append_record(aof, 3, argv, lens);
}

mr_status mr_aof_log_flushall(mr_aof *aof) {
    if (aof == NULL) {
        return MR_INVALID_ARG;
    }
    const void *argv[1] = {"FLUSHALL"};
    size_t lens[1] = {8};
    return append_record(aof, 1, argv, lens);
}

mr_status mr_aof_tick(mr_aof *aof) {
    if (aof == NULL || aof->policy != MR_AOF_FSYNC_EVERYSEC || !aof->dirty) {
        return MR_OK;
    }
    if (mr_clock_mono_ms(aof->clock) - aof->last_fsync_ms < AOF_EVERYSEC_MS) {
        return MR_OK;
    }
    return do_fsync(aof);
}

mr_status mr_aof_sync(mr_aof *aof) {
    if (aof == NULL) {
        return MR_INVALID_ARG;
    }
    return do_fsync(aof);
}

uint64_t mr_aof_size(const mr_aof *aof) { return aof->size; }

/* ---------- 재생(replay) ---------- */

typedef enum parse_result { PARSE_OK, PARSE_NEED_MORE, PARSE_BAD } parse_result;

typedef struct record {
    size_t argc;
    const unsigned char *argv[AOF_MAX_ARGC];
    size_t lens[AOF_MAX_ARGC];
} record;

/* "<prefix><digits>\r\n" 를 읽는다. */
static parse_result parse_len_line(const unsigned char *p, size_t n, size_t *i, char prefix,
                                   size_t *out) {
    size_t j = *i;
    if (j >= n) {
        return PARSE_NEED_MORE;
    }
    if (p[j] != (unsigned char)prefix) {
        return PARSE_BAD;
    }
    j++;
    size_t v = 0;
    size_t digits = 0;
    while (j < n && p[j] >= '0' && p[j] <= '9') {
        if (++digits > AOF_MAX_LEN_DIGITS) {
            return PARSE_BAD;
        }
        v = v * 10 + (size_t)(p[j] - '0');
        j++;
    }
    if (j >= n) {
        return PARSE_NEED_MORE;
    }
    if (p[j] != '\r' || digits == 0) {
        return PARSE_BAD;
    }
    if (j + 1 >= n) {
        return PARSE_NEED_MORE;
    }
    if (p[j + 1] != '\n') {
        return PARSE_BAD;
    }
    *i = j + 2;
    *out = v;
    return PARSE_OK;
}

static parse_result parse_record(const unsigned char *p, size_t n, record *rec,
                                 size_t *consumed) {
    size_t i = 0;
    size_t argc;
    parse_result r = parse_len_line(p, n, &i, '*', &argc);
    if (r != PARSE_OK) {
        return r;
    }
    if (argc == 0 || argc > AOF_MAX_ARGC) {
        return PARSE_BAD;
    }
    for (size_t a = 0; a < argc; a++) {
        size_t len;
        r = parse_len_line(p, n, &i, '$', &len);
        if (r != PARSE_OK) {
            return r;
        }
        if (len > AOF_MAX_BULK) {
            return PARSE_BAD;
        }
        if (n - i < len + 2) {
            return PARSE_NEED_MORE;
        }
        if (p[i + len] != '\r' || p[i + len + 1] != '\n') {
            return PARSE_BAD;
        }
        rec->argv[a] = p + i;
        rec->lens[a] = len;
        i += len + 2;
    }
    rec->argc = argc;
    *consumed = i;
    return PARSE_OK;
}

static bool arg_is(const record *rec, size_t i, const char *s) {
    size_t n = strlen(s);
    return rec->lens[i] == n && memcmp(rec->argv[i], s, n) == 0;
}

/* 양의 int64 10진수(선행 0 불가). */
static bool parse_pos_ms(const unsigned char *p, size_t n, int64_t *out) {
    if (n == 0 || n > 19 || p[0] < '1' || p[0] > '9') {
        return false;
    }
    int64_t v = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
        int d = p[i] - '0';
        if (v > (INT64_MAX - d) / 10) {
            return false;
        }
        v = v * 10 + d;
    }
    *out = v;
    return true;
}

/* 재생 시 "키 없음" 같은 결과는 정상(원본 시점과 같음). 메모리 부족만 그대로 전달. */
static mr_status replay_result(mr_status st) {
    if (st == MR_OK || st == MR_NOT_FOUND || st == MR_COND_FAILED) {
        return MR_OK;
    }
    return st == MR_NOMEM ? MR_NOMEM : MR_CORRUPT;
}

static mr_status apply_set(mr_store *store, const record *rec) {
    mr_set_opts o = {MR_SET_ALWAYS, MR_EXPIRE_NONE, 0};
    if (rec->argc == 5) {
        if (!arg_is(rec, 3, "PXAT") || !parse_pos_ms(rec->argv[4], rec->lens[4], &o.expire)) {
            return MR_CORRUPT;
        }
        o.expire_kind = MR_EXPIRE_PXAT;
    } else if (rec->argc != 3) {
        return MR_CORRUPT;
    }
    return replay_result(
        mr_store_set(store, rec->argv[1], rec->lens[1], rec->argv[2], rec->lens[2], &o));
}

static mr_status apply_record(mr_store *store, const record *rec) {
    const unsigned char *k = rec->argc > 1 ? rec->argv[1] : NULL;
    size_t kl = rec->argc > 1 ? rec->lens[1] : 0;
    if (arg_is(rec, 0, "SET")) {
        return apply_set(store, rec);
    }
    if (arg_is(rec, 0, "PEXPIREAT")) {
        int64_t at;
        if (rec->argc != 3 || !parse_pos_ms(rec->argv[2], rec->lens[2], &at)) {
            return MR_CORRUPT;
        }
        return replay_result(mr_store_pexpireat(store, k, kl, at));
    }
    if (arg_is(rec, 0, "FLUSHALL")) {
        if (rec->argc != 1) {
            return MR_CORRUPT;
        }
        mr_store_flushall(store);
        return MR_OK;
    }
    if (rec->argc != 2) {
        return MR_CORRUPT;
    }
    if (arg_is(rec, 0, "DEL")) {
        return replay_result(mr_store_del(store, k, kl));
    }
    if (arg_is(rec, 0, "PERSIST")) {
        return replay_result(mr_store_persist(store, k, kl));
    }
    if (arg_is(rec, 0, "INVALIDATE")) {
        return replay_result(mr_store_invalidate(store, k, kl, NULL));
    }
    if (arg_is(rec, 0, "INVALIDATEPREFIX")) {
        return replay_result(mr_store_invalidate_prefix(store, k, kl, NULL));
    }
    return MR_CORRUPT;
}

typedef struct reader {
    byte_buf b;
    size_t pos;        /* b.data 안에서 다음 레코드 시작 */
    uint64_t file_off; /* b.data[0] 의 파일 오프셋 */
} reader;

/* 다음 레코드에 더 많은 바이트가 필요할 때 버퍼를 정리·확장하고 읽는다.
 * 반환: 읽은 바이트 수(0 = EOF), 오류면 -1. */
static ssize_t reader_fill(int fd, reader *r, mr_status *st) {
    if (r->pos > 0) {
        memmove(r->b.data, r->b.data + r->pos, r->b.len - r->pos);
        r->b.len -= r->pos;
        r->file_off += r->pos;
        r->pos = 0;
    }
    *st = buf_reserve(&r->b, r->b.cap - r->b.len < AOF_READ_CHUNK ? AOF_READ_CHUNK : 0);
    if (*st != MR_OK) {
        return -1;
    }
    ssize_t n;
    do {
        n = read(fd, r->b.data + r->b.len, r->b.cap - r->b.len);
    } while (n < 0 && errno == EINTR);
    if (n < 0) {
        *st = MR_IO_ERROR;
        return -1;
    }
    r->b.len += (size_t)n;
    return n;
}

/* 불완전한 마지막 레코드를 잘라낸다. */
static mr_status truncate_tail(int fd, uint64_t good, uint64_t total, mr_aof_load_info *info) {
    if (ftruncate(fd, (off_t)good) != 0 || fsync_retry(fd) != 0) {
        return MR_IO_ERROR;
    }
    info->truncated_bytes = total - good;
    return MR_OK;
}

static mr_status replay(int fd, mr_store *store, mr_aof_load_info *info) {
    reader r = {{NULL, 0, 0}, 0, 0};
    mr_status st = MR_OK;
    for (;;) {
        record rec;
        size_t used;
        parse_result pr = parse_record(r.b.data + r.pos, r.b.len - r.pos, &rec, &used);
        if (pr == PARSE_OK) {
            st = apply_record(store, &rec);
            if (st != MR_OK) {
                break;
            }
            r.pos += used;
            info->records++;
            continue;
        }
        if (pr == PARSE_BAD) {
            st = MR_CORRUPT;
            break;
        }
        ssize_t n = reader_fill(fd, &r, &st);
        if (n < 0) {
            break;
        }
        if (n == 0) {
            /* EOF 인데 레코드가 덜 끝났다 = 쓰는 도중 죽은 흔적. 완결 레코드까지만 남긴다. */
            if (r.b.len > 0) {
                st = truncate_tail(fd, r.file_off, r.file_off + r.b.len, info);
            }
            break;
        }
    }
    free(r.b.data);
    return st;
}

/* ---------- 열기 / 닫기 ---------- */

static void aof_free(mr_aof *aof) {
    if (aof->fd >= 0) {
        close(aof->fd);
    }
    free(aof->buf.data);
    free(aof->path);
    free(aof);
}

static mr_status aof_load(mr_aof *aof, mr_store *store, mr_aof_load_info *info) {
    (void)mr_store_set_loading(store, true);
    mr_status st = replay(aof->fd, store, info);
    info->expired_dropped = mr_store_set_loading(store, false);
    if (st != MR_OK) {
        return st;
    }
    struct stat sb;
    if (fstat(aof->fd, &sb) != 0) {
        return MR_IO_ERROR;
    }
    aof->size = (uint64_t)sb.st_size;
    return fsync_parent_dir(aof->path);
}

mr_status mr_aof_open(const mr_aof_opts *opts, mr_store *store, mr_aof **out,
                      mr_aof_load_info *info) {
    mr_aof_load_info local;
    if (opts == NULL || opts->path == NULL || store == NULL || out == NULL ||
        (opts->fsync != MR_AOF_FSYNC_ALWAYS && opts->fsync != MR_AOF_FSYNC_EVERYSEC &&
         opts->fsync != MR_AOF_FSYNC_NO)) {
        return MR_INVALID_ARG;
    }
    *out = NULL;
    if (info == NULL) {
        info = &local;
    }
    memset(info, 0, sizeof(*info));

    mr_aof *aof = calloc(1, sizeof(*aof));
    if (aof == NULL) {
        return MR_NOMEM;
    }
    aof->fd = -1;
    size_t plen = strlen(opts->path);
    aof->path = malloc(plen + 1);
    if (aof->path == NULL) {
        aof_free(aof);
        return MR_NOMEM;
    }
    memcpy(aof->path, opts->path, plen + 1);
    aof->policy = opts->fsync;
    aof->clock = opts->clock != NULL ? opts->clock : mr_clock_system();

    /* O_APPEND: 읽기는 처음부터, 쓰기는 항상 끝에. 재생 후 오프셋을 옮길 필요가 없다. */
    aof->fd = open(aof->path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (aof->fd < 0) {
        aof_free(aof);
        return MR_IO_ERROR;
    }
    mr_status st = aof_load(aof, store, info);
    if (st != MR_OK) {
        aof_free(aof);
        return st;
    }
    aof->last_fsync_ms = mr_clock_mono_ms(aof->clock);
    *out = aof;
    return MR_OK;
}

mr_status mr_aof_close(mr_aof *aof) {
    if (aof == NULL) {
        return MR_OK;
    }
    mr_status st = MR_OK;
    if (aof->dirty && aof->policy != MR_AOF_FSYNC_NO && fsync_retry(aof->fd) != 0) {
        st = MR_IO_ERROR;
    }
    if (close(aof->fd) != 0 && st == MR_OK) {
        st = MR_IO_ERROR;
    }
    aof->fd = -1;
    aof_free(aof);
    return st;
}

/* ---------- 압축(rewrite) ---------- */

typedef struct rewrite_ctx {
    int fd;
    byte_buf *buf;
    uint64_t written;
    mr_status st;
} rewrite_ctx;

static mr_status rewrite_flush(rewrite_ctx *c) {
    mr_status st = write_all(c->fd, c->buf->data, c->buf->len);
    if (st == MR_OK) {
        c->written += c->buf->len;
        c->buf->len = 0;
    }
    return st;
}

static bool rewrite_visit(const void *key, size_t klen, const void *val, size_t vlen,
                          int64_t expire_at_ms, void *ud) {
    rewrite_ctx *c = ud;
    char ms[24];
    int n = fmt_i64(ms, sizeof(ms), expire_at_ms);
    const void *argv[5] = {"SET", key, val, "PXAT", ms};
    size_t lens[5] = {3, klen, vlen, 4, (size_t)n};
    c->st = encode_cmd(c->buf, expire_at_ms == 0 ? 3 : 5, argv, lens);
    if (c->st == MR_OK && c->buf->len >= AOF_REWRITE_FLUSH) {
        c->st = rewrite_flush(c);
    }
    return c->st == MR_OK;
}

static mr_status rewrite_into(int fd, mr_store *store, byte_buf *buf, uint64_t *written) {
    rewrite_ctx c = {fd, buf, 0, MR_OK};
    buf->len = 0;
    mr_status st = mr_store_keys(store, NULL, 0, rewrite_visit, &c);
    if (st == MR_OK) {
        st = c.st;
    }
    if (st == MR_OK && buf->len > 0) {
        st = rewrite_flush(&c);
    }
    if (st == MR_OK && fsync_retry(fd) != 0) {
        st = MR_IO_ERROR;
    }
    *written = c.written;
    return st;
}

mr_status mr_aof_rewrite(mr_aof *aof, mr_store *store) {
    if (aof == NULL || store == NULL) {
        return MR_INVALID_ARG;
    }
    size_t plen = strlen(aof->path);
    size_t slen = sizeof(AOF_TMP_SUFFIX);
    char *tmp = malloc(plen + slen);
    if (tmp == NULL) {
        return MR_NOMEM;
    }
    memcpy(tmp, aof->path, plen);
    memcpy(tmp + plen, AOF_TMP_SUFFIX, slen);

    /* 임시 파일의 fd 를 그대로 새 AOF 의 fd 로 쓴다: rename 뒤 다시 여는 단계가
     * 없으므로 "rename 은 됐는데 재오픈 실패" 같은 어중간한 상태가 생기지 않는다. */
    int fd = open(tmp, O_RDWR | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        free(tmp);
        return MR_IO_ERROR;
    }
    uint64_t written = 0;
    mr_status st = rewrite_into(fd, store, &aof->buf, &written);
    if (st == MR_OK && rename(tmp, aof->path) != 0) {
        st = MR_IO_ERROR;
    }
    if (st != MR_OK) {
        close(fd);
        unlink(tmp);
        free(tmp);
        return st;
    }
    free(tmp);
    close(aof->fd);
    aof->fd = fd;
    aof->size = written;
    aof->dirty = false;
    aof->broken = false;
    aof->last_fsync_ms = mr_clock_mono_ms(aof->clock);
    /* 여기서 실패하면 이미 새 파일로 전환된 상태로 오류만 보고한다. */
    return fsync_parent_dir(aof->path);
}
