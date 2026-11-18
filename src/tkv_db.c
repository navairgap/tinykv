#include "tinykv/db.h"
#include "tkv_index.h"
#include "tkv_io.h"
#include "tkv_log.h"
#include "tkv_mutex.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TINYKV_DEFAULT_MAX_KEY 4096u
#define TINYKV_DEFAULT_MAX_VALUE 1048576u
#define TINYKV_PATH_MAX 4096

struct tinykv_db {
    tkv_mutex mutex;
    char *path;
    int fd;
    int lock_fd;
    tkv_index index;
    tkv_log_limits limits;
    tinykv_options options;
    uint64_t file_size;      /* logical end of the log, including buffered */
    uint64_t flushed_size;   /* bytes actually handed to the descriptor */
    uint8_t *wbuf;           /* pending append bytes */
    size_t wlen, wcap;
    int torn_tail;           /* a torn tail exists; reclaimed on next write */
    uint64_t tail_last_good; /* offset to truncate to before appending */
    uint64_t puts, deletes, gets, compactions;
};

/* Appends accumulate here so bulk loads pay syscalls per 64 KiB instead
 * of per record; reads, syncs, and close flush first. */
#define TINYKV_WRITE_BUFFER 65536u

/* Locking is logically const: read APIs mutate only cache state, so the
 * const surface stays intact for callers. */
static tkv_mutex *handle_mutex(const tinykv_db *db)
{
    return (tkv_mutex *)&db->mutex;
}

const char *tinykv_status_string(tinykv_status status)
{
    switch (status) {
    case TINYKV_OK: return "ok";
    case TINYKV_ERR_IO: return "filesystem error";
    case TINYKV_ERR_NOMEM: return "out of memory";
    case TINYKV_ERR_INVALID: return "invalid argument";
    case TINYKV_ERR_NOT_FOUND: return "key not found";
    case TINYKV_ERR_CORRUPT: return "database corruption";
    case TINYKV_ERR_LOCKED: return "database is locked by another process";
    case TINYKV_ERR_LIMIT: return "size limit exceeded";
    case TINYKV_ERR_TORN_TAIL: return "torn tail (repair available)";
    }
    return "unknown status";
}

const char *tinykv_version_string(void)
{
    return TINYKV_VERSION_STRING;
}

static tinykv_status options_check(const tinykv_options *options)
{
    if (options->max_key_length == 0 || options->max_value_length == 0)
        return TINYKV_ERR_INVALID;
    if (options->sync != TINYKV_SYNC_NONE &&
        options->sync != TINYKV_SYNC_ON_CLOSE &&
        options->sync != TINYKV_SYNC_EVERY_WRITE)
        return TINYKV_ERR_INVALID;
    if (!(options->auto_compact_dead_ratio >= 0.0 &&
          options->auto_compact_dead_ratio <= 1.0))
        return TINYKV_ERR_INVALID; /* also rejects NaN */
    return TINYKV_OK;
}

static tinykv_options options_default(void)
{
    tinykv_options defaults;
    defaults.max_key_length = TINYKV_DEFAULT_MAX_KEY;
    defaults.max_value_length = TINYKV_DEFAULT_MAX_VALUE;
    defaults.sync = TINYKV_SYNC_EVERY_WRITE;
    defaults.create_if_missing = 1;
    defaults.read_only = 0;
    defaults.lock = 1;
    defaults.auto_compact_dead_ratio = 0.0;
    return defaults;
}

struct replay_state {
    tinykv_db *db;
    int oom;
};

/* Replay one validated record into the in-memory index. */
static int replay_record(const tinykv_record_view *view, uint64_t offset,
                         void *user)
{
    struct replay_state *state = (struct replay_state *)user;
    tinykv_db *db = state->db;
    if (view->type == TINYKV_RECORD_PUT) {
        if (tkv_index_put(&db->index, view->key, view->key_length, offset,
                          TINYKV_RECORD_HEADER_SIZE + view->key_length +
                              view->value_length,
                          view->value_length) != 0) {
            state->oom = 1;
            return 1;
        }
        return 0;
    }
    if (view->type == TINYKV_RECORD_DELETE)
        tkv_index_remove(&db->index, view->key, view->key_length);
    return 0;
}

static tinykv_status status_from_codec(tinykv_record_status status)
{
    switch (status) {
    case TINYKV_RECORD_OK: return TINYKV_OK;
    case TINYKV_RECORD_INVALID_ARGUMENT:
    case TINYKV_RECORD_INVALID_TYPE:
    case TINYKV_RECORD_INVALID_FORMAT: return TINYKV_ERR_INVALID;
    case TINYKV_RECORD_LIMIT_EXCEEDED: return TINYKV_ERR_LIMIT;
    case TINYKV_RECORD_TRUNCATED:
    case TINYKV_RECORD_BAD_CHECKSUM: return TINYKV_ERR_CORRUPT;
    case TINYKV_RECORD_OUT_OF_MEMORY: return TINYKV_ERR_NOMEM;
    }
    return TINYKV_ERR_INVALID;
}

static tinykv_status write_arguments_status(const tinykv_db *db,
                                            const void *key,
                                            size_t key_length,
                                            const void *value,
                                            size_t value_length)
{
    if (db == NULL || db->fd < 0) return TINYKV_ERR_INVALID;
    if (key == NULL && key_length != 0) return TINYKV_ERR_INVALID;
    if (value == NULL && value_length != 0) return TINYKV_ERR_INVALID;
    if (key_length > (size_t)db->limits.max_key_length ||
        key_length > UINT32_MAX)
        return TINYKV_ERR_LIMIT;
    if (value_length > (size_t)db->limits.max_value_length ||
        value_length > UINT32_MAX)
        return TINYKV_ERR_LIMIT;
    return TINYKV_OK;
}

/* Drop a torn tail before appending; bytes after the last validated
 * record can never be appended over without corrupting the log. */
static tinykv_status reclaim_torn_tail(tinykv_db *db)
{
    if (!db->torn_tail) return TINYKV_OK;
    if (tkv_truncate_file(db->fd, db->tail_last_good) != 0)
        return TINYKV_ERR_IO;
    if (tkv_fsync_file(db->fd) != 0) return TINYKV_ERR_IO;
    db->file_size = db->tail_last_good;
    db->flushed_size = db->tail_last_good;
    db->torn_tail = 0;
    return TINYKV_OK;
}

/* Hand pending append bytes to the descriptor; data reaches the page
 * cache but is only durable once an fsync follows. Positional writes:
 * the descriptor's file position belongs to nobody in particular. */
static tinykv_status flush_wbuf(tinykv_db *db)
{
    if (db->wlen == 0) return TINYKV_OK;
    if (tkv_pwrite_all(db->fd, db->wbuf, db->wlen, db->flushed_size) != 0)
        return TINYKV_ERR_IO;
    db->flushed_size += db->wlen;
    db->wlen = 0;
    return TINYKV_OK;
}

/* Reads never look past the flushed region, so any access to a record
 * still sitting in the write buffer flushes first. */
static tinykv_status flush_for_read(const tinykv_db *db, uint64_t offset,
                                    uint32_t record_length)
{
    return offset + record_length > db->flushed_size
               ? flush_wbuf((tinykv_db *)db)
               : TINYKV_OK;
}

static tinykv_status write_record(tinykv_db *db, uint8_t type,
                                  const void *key, size_t key_length,
                                  const void *value, size_t value_length)
{
    tinykv_status status;
    tinykv_record_status encoded_status;
    uint8_t *encoded = NULL;
    size_t encoded_length = 0;
    uint64_t offset;

    status = reclaim_torn_tail(db);
    if (status != TINYKV_OK) return status;
    encoded_status = tinykv_record_encode(type, (const uint8_t *)key,
                                          key_length, (const uint8_t *)value,
                                          value_length,
                                          db->limits.max_key_length,
                                          db->limits.max_value_length,
                                          &encoded, &encoded_length);
    if (encoded_status != TINYKV_RECORD_OK) {
        free(encoded);
        return status_from_codec(encoded_status);
    }
    offset = db->file_size;
    if (db->wlen + encoded_length > db->wcap) {
        size_t larger = db->wcap ? db->wcap : TINYKV_WRITE_BUFFER;
        uint8_t *grown;
        while (larger < db->wlen + encoded_length) larger *= 2;
        grown = (uint8_t *)realloc(db->wbuf, larger);
        if (grown == NULL) {
            free(encoded);
            return TINYKV_ERR_NOMEM;
        }
        db->wbuf = grown;
        db->wcap = larger;
    }
    memcpy(db->wbuf + db->wlen, encoded, encoded_length);
    db->wlen += encoded_length;
    free(encoded);

    if (type == TINYKV_RECORD_PUT) {
        if (tkv_index_put(&db->index, (const uint8_t *)key, key_length,
                          offset, (uint32_t)encoded_length,
                          (uint32_t)value_length) != 0) {
            db->wlen -= encoded_length; /* index is authoritative */
            return TINYKV_ERR_NOMEM;
        }
        db->puts++;
    } else {
        tkv_index_remove(&db->index, (const uint8_t *)key, key_length);
        db->deletes++;
    }
    db->file_size = offset + encoded_length;

    if (db->options.sync == TINYKV_SYNC_EVERY_WRITE) {
        status = flush_wbuf(db);
        if (status != TINYKV_OK) return status;
        if (tkv_fsync_file(db->fd) != 0) return TINYKV_ERR_IO;
    } else if (db->wlen >= TINYKV_WRITE_BUFFER) {
        status = flush_wbuf(db);
        if (status != TINYKV_OK) return status;
    }
    return TINYKV_OK;
}

tinykv_status tinykv_put(tinykv_db *db, const void *key, size_t key_length,
                         const void *value, size_t value_length)
{
    tinykv_status status =
        write_arguments_status(db, key, key_length, value, value_length);
    if (status != TINYKV_OK) return status;
    if (db->options.read_only) return TINYKV_ERR_INVALID;
    tkv_mutex_lock(&db->mutex);
    status = write_record(db, TINYKV_RECORD_PUT, key, key_length, value,
                          value_length);
    tkv_mutex_unlock(&db->mutex);
    return status;
}

tinykv_status tinykv_delete(tinykv_db *db, const void *key, size_t key_length)
{
    tinykv_status status =
        write_arguments_status(db, key, key_length, NULL, 0);
    if (status != TINYKV_OK) return status;
    if (db->options.read_only) return TINYKV_ERR_INVALID;
    tkv_mutex_lock(&db->mutex);
    if (tkv_index_find(&db->index, (const uint8_t *)key, key_length) == NULL) {
        tkv_mutex_unlock(&db->mutex);
        return TINYKV_ERR_NOT_FOUND;
    }
    status = write_record(db, TINYKV_RECORD_DELETE, key, key_length, NULL, 0);
    tkv_mutex_unlock(&db->mutex);
    return status;
}

tinykv_status tinykv_get(const tinykv_db *db, const void *key,
                         size_t key_length, void **out_value,
                         size_t *out_value_length)
{
    const tkv_entry *entry;
    uint8_t *record = NULL;
    tinykv_record_view view;
    size_t consumed = 0;
    void *value;
    tinykv_record_status status;
    tinykv_status flush_status;

    if (out_value == NULL || out_value_length == NULL)
        return TINYKV_ERR_INVALID;
    *out_value = NULL;
    *out_value_length = 0;
    if (db == NULL || db->fd < 0 || (key == NULL && key_length != 0))
        return TINYKV_ERR_INVALID;

    tkv_mutex_lock(handle_mutex(db));
    entry = tkv_index_find(&db->index, (const uint8_t *)key, key_length);
    if (entry == NULL) {
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_NOT_FOUND;
    }

    flush_status = flush_for_read(db, entry->offset, entry->record_length);
    if (flush_status != TINYKV_OK) {
        tkv_mutex_unlock(handle_mutex(db));
        return flush_status;
    }

    record = (uint8_t *)malloc(entry->record_length);
    if (record == NULL) {
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_NOMEM;
    }
    if (tkv_pread_all(db->fd, record, entry->record_length,
                      entry->offset) != 0) {
        free(record);
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_IO;
    }
    status = tinykv_record_decode(record, entry->record_length,
                                  db->limits.max_key_length,
                                  db->limits.max_value_length, &view,
                                  &consumed);
    if (status != TINYKV_RECORD_OK || view.type != TINYKV_RECORD_PUT ||
        view.key_length != key_length ||
        (key_length != 0 && memcmp(view.key, key, key_length) != 0)) {
        free(record);
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_CORRUPT;
    }
    value = malloc(view.value_length ? view.value_length : 1);
    if (value == NULL) {
        free(record);
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_NOMEM;
    }
    if (view.value_length != 0)
        memcpy(value, view.value, view.value_length);
    free(record);
    *out_value = value;
    *out_value_length = view.value_length;
    tkv_mutex_unlock(handle_mutex(db));
    return TINYKV_OK;
}

static void db_free(tinykv_db *db)
{
    if (db == NULL) return;
    tkv_mutex_destroy(&db->mutex);
    tkv_index_free(&db->index);
    free(db->wbuf);
    free(db->path);
    free(db);
}

tinykv_status tinykv_open(const char *path, const tinykv_options *options,
                          tinykv_db **out_db)
{
    tinykv_options effective;
    tkv_log_scan_result scan;
    tinykv_db *db = NULL;
    char lock_path[TINYKV_PATH_MAX];
    tinykv_status status = TINYKV_OK;
    int fd = -1, lock_fd = -1, created = 0;

    if (out_db == NULL || path == NULL || path[0] == '\0')
        return TINYKV_ERR_INVALID;
    *out_db = NULL;
    effective = options == NULL ? options_default() : *options;
    if (strlen(path) >= sizeof(lock_path) - 8) return TINYKV_ERR_INVALID;
    status = options_check(&effective);
    if (status != TINYKV_OK) return status;

    if (!effective.read_only && !effective.create_if_missing) {
        struct stat st;
        if (stat(path, &st) != 0) return TINYKV_ERR_IO;
    }

    fd = tkv_open_log(path, effective.read_only, &created);
    if (fd < 0) return TINYKV_ERR_IO;

    if (!effective.read_only && effective.lock) {
        if (tkv_sidecar_path(path, ".lock", lock_path, sizeof(lock_path)) != 0) {
            status = TINYKV_ERR_INVALID;
            goto fail;
        }
        do
            lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        while (lock_fd < 0 && errno == EINTR);
        if (lock_fd < 0 || tkv_lock_try(lock_fd, 1) != 0) {
            status = TINYKV_ERR_LOCKED;
            goto fail;
        }
    }

    {
        tkv_log_limits limits;
        struct replay_state replay;
        limits.max_key_length = effective.max_key_length;
        limits.max_value_length = effective.max_value_length;
        db = (tinykv_db *)calloc(1, sizeof(*db));
        if (db == NULL) {
            status = TINYKV_ERR_NOMEM;
            goto fail;
        }
        if (tkv_mutex_init(&db->mutex) != 0) {
            status = TINYKV_ERR_NOMEM;
            goto fail;
        }
        db->fd = fd;
        db->lock_fd = lock_fd;
        db->limits = limits;
        db->options = effective;
        if (tkv_index_init(&db->index) != 0) {
            status = TINYKV_ERR_NOMEM;
            goto fail;
        }
        replay.db = db;
        replay.oom = 0;
        if (tkv_log_scan(fd, &limits, replay_record, &replay, &scan) != 0) {
            status = TINYKV_ERR_IO;
            goto fail;
        }
        if (replay.oom) {
            status = TINYKV_ERR_NOMEM;
            goto fail;
        }
        if (scan.stop_status != TINYKV_RECORD_OK &&
            scan.stop_status != TINYKV_RECORD_TRUNCATED) {
            status = TINYKV_ERR_CORRUPT;
            goto fail;
        }
        db->file_size = scan.total_bytes;
        db->flushed_size = scan.total_bytes;
        db->torn_tail = scan.stop_status == TINYKV_RECORD_TRUNCATED;
        db->tail_last_good = scan.last_good_offset;
    }

    db->path = strdup(path);
    if (db->path == NULL) {
        status = TINYKV_ERR_NOMEM;
        goto fail;
    }
    *out_db = db;
    return TINYKV_OK;

fail:
    if (lock_fd >= 0) close(lock_fd);
    if (fd >= 0) close(fd);
    db_free(db);
    return status;
}

struct bytes_state {
    uint64_t total;
};

static void add_record_bytes(tkv_entry *entry, void *user)
{
    struct bytes_state *state = (struct bytes_state *)user;
    state->total += entry->record_length;
}

tinykv_status tinykv_sync(tinykv_db *db)
{
    tinykv_status status;
    if (db == NULL || db->fd < 0) return TINYKV_ERR_INVALID;
    if (db->options.read_only) return TINYKV_ERR_INVALID;
    tkv_mutex_lock(&db->mutex);
    status = flush_wbuf(db);
    if (status == TINYKV_OK && tkv_fsync_file(db->fd) != 0)
        status = TINYKV_ERR_IO;
    tkv_mutex_unlock(&db->mutex);
    return status;
}

tinykv_status tinykv_get_stats(const tinykv_db *db, tinykv_stats *out_stats)
{
    struct bytes_state bytes = {0};
    if (db == NULL || out_stats == NULL) return TINYKV_ERR_INVALID;
    tkv_mutex_lock(handle_mutex(db));
    memset(out_stats, 0, sizeof(*out_stats));
    tkv_index_foreach(&db->index, add_record_bytes, &bytes);
    out_stats->records = (uint64_t)tkv_index_count(&db->index);
    out_stats->file_size = db->file_size;
    out_stats->live_bytes = bytes.total;
    out_stats->dead_bytes = db->file_size > bytes.total
                                ? db->file_size - bytes.total
                                : 0;
    out_stats->discarded_bytes =
        db->torn_tail ? db->file_size - db->tail_last_good : 0;
    out_stats->puts = db->puts;
    out_stats->deletes = db->deletes;
    out_stats->compactions = db->compactions;
    tkv_mutex_unlock(handle_mutex(db));
    return TINYKV_OK;
}

struct compact_entry {
    tkv_entry *entry;
    uint64_t new_offset;
};

struct compact_state {
    tinykv_db *db;
    int tmp_fd;
    uint64_t offset;
    int failed;
    struct compact_entry *list;
    size_t count, capacity;
};

/* Copy one live record to the compaction file, remembering its new
 * offset. Index updates are deferred until the swap succeeds so a
 * failure leaves both the original file and the index untouched. */
static void compact_visit(tkv_entry *entry, void *user)
{
    struct compact_state *state = (struct compact_state *)user;
    uint8_t *record;
    tinykv_record_view view;
    size_t consumed = 0;

    if (state->failed) return;
    record = (uint8_t *)malloc(entry->record_length);
    if (record == NULL) {
        state->failed = 1;
        return;
    }
    if (tkv_pread_all(state->db->fd, record, entry->record_length,
                      entry->offset) != 0 ||
        tinykv_record_decode(record, entry->record_length,
                             state->db->limits.max_key_length,
                             state->db->limits.max_value_length, &view,
                             &consumed) != TINYKV_RECORD_OK ||
        tkv_write_all(state->tmp_fd, record, entry->record_length) != 0) {
        free(record);
        state->failed = 1;
        return;
    }
    free(record);
    if (state->count == state->capacity) {
        size_t larger = state->capacity ? state->capacity * 2 : 64;
        struct compact_entry *grown =
            (struct compact_entry *)realloc(state->list,
                                            larger * sizeof(*grown));
        if (grown == NULL) {
            state->failed = 1;
            return;
        }
        state->list = grown;
        state->capacity = larger;
    }
    state->list[state->count].entry = entry;
    state->list[state->count].new_offset = state->offset;
    state->count++;
    state->offset += entry->record_length;
}

tinykv_status tinykv_compact(tinykv_db *db)
{
    char tmp_path[TINYKV_PATH_MAX];
    struct compact_state state;
    tinykv_status status;
    int tmp_fd = -1;
    size_t i;

    if (db == NULL || db->fd < 0) return TINYKV_ERR_INVALID;
    if (db->options.read_only) return TINYKV_ERR_INVALID;
    if (tkv_sidecar_path(db->path, ".compact", tmp_path, sizeof(tmp_path)) !=
        0)
        return TINYKV_ERR_INVALID;

    tkv_mutex_lock(&db->mutex);
    status = reclaim_torn_tail(db);
    if (status != TINYKV_OK) {
        tkv_mutex_unlock(&db->mutex);
        return status;
    }
    status = flush_wbuf(db); /* reads below pread the flushed region */
    if (status != TINYKV_OK) {
        tkv_mutex_unlock(&db->mutex);
        return status;
    }

    memset(&state, 0, sizeof(state));
    state.db = db;
    (void)unlink(tmp_path); /* a stale temp from an interrupted compaction */
    do
        tmp_fd = open(tmp_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    while (tmp_fd < 0 && errno == EINTR);
    if (tmp_fd < 0) return TINYKV_ERR_IO;
    state.tmp_fd = tmp_fd;

    tkv_index_foreach_mutable(&db->index, compact_visit, &state);
    if (state.failed || tkv_fsync_file(tmp_fd) != 0) {
        close(tmp_fd);
        (void)unlink(tmp_path);
        free(state.list);
        tkv_mutex_unlock(&db->mutex);
        return TINYKV_ERR_IO;
    }
    close(tmp_fd);
    /* Atomic swap: the compacted file replaces the log only after the
     * replacement itself is synced, per docs/repair-mode-safety.md. */
    if (tkv_rename_sync(tmp_path, db->path) != 0) {
        (void)unlink(tmp_path);
        free(state.list);
        tkv_mutex_unlock(&db->mutex);
        return TINYKV_ERR_IO;
    }
    close(db->fd);
    db->fd = tkv_open_log(db->path, 0, NULL);
    if (db->fd < 0) {
        free(state.list);
        tkv_mutex_unlock(&db->mutex);
        return TINYKV_ERR_IO;
    }
    /* Only now repoint the index at the compacted layout. */
    for (i = 0; i < state.count; ++i)
        state.list[i].entry->offset = state.list[i].new_offset;
    free(state.list);
    db->file_size = state.offset;
    db->flushed_size = state.offset;
    db->wlen = 0;
    db->compactions++;
    tkv_mutex_unlock(&db->mutex);
    return TINYKV_OK;
}

static tinykv_status classify_log(const char *path, uint32_t max_key_length,
                                  uint32_t max_value_length,
                                  uint64_t *out_last_good,
                                  uint64_t *out_total)
{
    tkv_log_limits limits;
    tkv_log_scan_result scan;
    tinykv_status status;
    int fd;
    limits.max_key_length = max_key_length;
    limits.max_value_length = max_value_length;
    fd = tkv_open_log(path, 1, NULL);
    if (fd < 0) return TINYKV_ERR_IO;
    if (tkv_log_scan(fd, &limits, NULL, NULL, &scan) != 0) {
        close(fd);
        return TINYKV_ERR_IO;
    }
    close(fd);
    if (scan.stop_status == TINYKV_RECORD_OK)
        status = TINYKV_OK;
    else if (scan.stop_status == TINYKV_RECORD_TRUNCATED)
        status = TINYKV_ERR_TORN_TAIL;
    else
        status = TINYKV_ERR_CORRUPT;
    *out_last_good = scan.last_good_offset;
    *out_total = scan.total_bytes;
    return status;
}

static int copy_prefix(const char *from, const char *to, uint64_t length)
{
    static uint8_t buffer[65536];
    int src = -1, dst = -1, result = -1;
    do
        src = open(from, O_RDONLY | O_CLOEXEC);
    while (src < 0 && errno == EINTR);
    if (src < 0) return -1;
    do
        dst = open(to, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    while (dst < 0 && errno == EINTR);
    if (dst < 0) goto out;
    while (length != 0) {
        size_t want = length < sizeof(buffer) ? (size_t)length : sizeof(buffer);
        ssize_t got;
        do
            got = read(src, buffer, want);
        while (got < 0 && errno == EINTR);
        if (got <= 0) goto out;
        if (tkv_write_all(dst, buffer, (size_t)got) != 0) goto out;
        length -= (uint64_t)got;
    }
    result = tkv_fsync_file(dst);
out:
    if (dst >= 0) close(dst);
    close(src);
    return result;
}

tinykv_status tinykv_verify(const char *path, uint32_t max_key_length,
                            uint32_t max_value_length,
                            uint64_t *out_last_good_offset)
{
    uint64_t last_good = 0, total = 0;
    tinykv_status status;
    if (path == NULL || path[0] == '\0' || out_last_good_offset == NULL)
        return TINYKV_ERR_INVALID;
    if (max_key_length == 0 || max_value_length == 0)
        return TINYKV_ERR_INVALID;
    *out_last_good_offset = 0;
    status = classify_log(path, max_key_length, max_value_length, &last_good,
                          &total);
    if (status == TINYKV_OK || status == TINYKV_ERR_TORN_TAIL ||
        status == TINYKV_ERR_CORRUPT)
        *out_last_good_offset = last_good;
    return status;
}

tinykv_status tinykv_repair(const char *path, uint32_t max_key_length,
                            uint32_t max_value_length, int dry_run,
                            uint64_t *out_last_good_offset)
{
    char bak_path[TINYKV_PATH_MAX], repair_path[TINYKV_PATH_MAX],
        lock_path[TINYKV_PATH_MAX];
    uint64_t last_good = 0, total = 0;
    tinykv_status status;
    int lock_fd = -1;
    int held_lock = 0;

    if (path == NULL || path[0] == '\0' || out_last_good_offset == NULL)
        return TINYKV_ERR_INVALID;
    if (max_key_length == 0 || max_value_length == 0)
        return TINYKV_ERR_INVALID;
    if (strlen(path) >= sizeof(bak_path) - 32) return TINYKV_ERR_INVALID;
    *out_last_good_offset = 0;

    /* Repair must not race an open writer: take the same lock an open
     * handle would hold, but never wait for it. */
    if (tkv_sidecar_path(path, ".lock", lock_path, sizeof(lock_path)) == 0) {
        do
            lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        while (lock_fd < 0 && errno == EINTR);
        if (lock_fd >= 0 && tkv_lock_try(lock_fd, 1) == 0)
            held_lock = 1;
        else if (lock_fd >= 0) {
            close(lock_fd);
            return TINYKV_ERR_LOCKED;
        }
        /* A missing lock directory entry is fine: no writer is present. */
    }

    status = classify_log(path, max_key_length, max_value_length, &last_good,
                          &total);
    if (status == TINYKV_OK || status == TINYKV_ERR_IO) {
        if (held_lock) tkv_unlock(lock_fd);
        if (lock_fd >= 0) close(lock_fd);
        *out_last_good_offset = last_good;
        return status; /* already clean, nothing to do */
    }
    *out_last_good_offset = last_good;
    if (dry_run) {
        if (held_lock) tkv_unlock(lock_fd);
        if (lock_fd >= 0) close(lock_fd);
        return status;
    }

    if (tkv_sidecar_path(path, ".bak", bak_path, sizeof(bak_path)) != 0 ||
        tkv_sidecar_path(path, ".repair", repair_path, sizeof(repair_path)) !=
            0) {
        status = TINYKV_ERR_INVALID;
        goto out;
    }
    /* Preserve the original before touching anything (step 4). */
    (void)unlink(bak_path);
    if (tkv_copy_file(path, bak_path) != 0) {
        status = TINYKV_ERR_IO;
        goto out;
    }
    /* Write the retained prefix to a new file, then verify it. */
    (void)unlink(repair_path);
    if (copy_prefix(path, repair_path, last_good) != 0) {
        (void)unlink(repair_path);
        status = TINYKV_ERR_IO;
        goto out;
    }
    {
        uint64_t repaired_good = 0, repaired_total = 0;
        tinykv_status check = classify_log(repair_path, max_key_length,
                                           max_value_length, &repaired_good,
                                           &repaired_total);
        if (check != TINYKV_OK || repaired_total != last_good) {
            (void)unlink(repair_path);
            status = TINYKV_ERR_IO;
            goto out;
        }
    }
    /* Only replace the original after the new file is verified. */
    if (tkv_rename_sync(repair_path, path) != 0) {
        (void)unlink(repair_path);
        status = TINYKV_ERR_IO;
        goto out;
    }
    status = TINYKV_OK;
out:
    if (held_lock) tkv_unlock(lock_fd);
    if (lock_fd >= 0) close(lock_fd);
    return status;
}

tinykv_status tinykv_get_into(const tinykv_db *db, const void *key,
                              size_t key_length, void *buffer,
                              size_t capacity, size_t *out_value_length)
{
    const tkv_entry *entry;
    uint8_t *record = NULL;
    tinykv_record_view view;
    size_t consumed = 0;
    tinykv_record_status status;
    tinykv_status flush_status;

    if (out_value_length == NULL) return TINYKV_ERR_INVALID;
    *out_value_length = 0;
    if (db == NULL || db->fd < 0 || (key == NULL && key_length != 0) ||
        (buffer == NULL && capacity != 0))
        return TINYKV_ERR_INVALID;

    tkv_mutex_lock(handle_mutex(db));
    entry = tkv_index_find(&db->index, (const uint8_t *)key, key_length);
    if (entry == NULL) {
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_NOT_FOUND;
    }
    *out_value_length = entry->value_length;
    if (entry->value_length > capacity) {
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_LIMIT;
    }
    flush_status = flush_for_read(db, entry->offset, entry->record_length);
    if (flush_status != TINYKV_OK) {
        tkv_mutex_unlock(handle_mutex(db));
        return flush_status;
    }

    record = (uint8_t *)malloc(entry->record_length);
    if (record == NULL) {
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_NOMEM;
    }
    if (tkv_pread_all(db->fd, record, entry->record_length,
                      entry->offset) != 0) {
        free(record);
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_IO;
    }
    status = tinykv_record_decode(record, entry->record_length,
                                  db->limits.max_key_length,
                                  db->limits.max_value_length, &view,
                                  &consumed);
    if (status != TINYKV_RECORD_OK || view.type != TINYKV_RECORD_PUT ||
        view.key_length != key_length ||
        (key_length != 0 && memcmp(view.key, key, key_length) != 0)) {
        free(record);
        tkv_mutex_unlock(handle_mutex(db));
        return TINYKV_ERR_CORRUPT;
    }
    if (view.value_length != 0)
        memcpy(buffer, view.value, view.value_length);
    free(record);
    tkv_mutex_unlock(handle_mutex(db));
    return TINYKV_OK;
}

struct iter_key {
    uint8_t *key;
    uint32_t key_length;
};

struct tinykv_iter {
    tinykv_db *db;
    struct iter_key *keys;
    size_t count, position;
    uint8_t *value;
    size_t value_length;
    uint8_t *prefix; /* optional key filter, NULL = whole keyspace */
    uint32_t prefix_length;
    int oom;
};

static int iter_key_compare(const void *left, const void *right)
{
    const struct iter_key *a = (const struct iter_key *)left;
    const struct iter_key *b = (const struct iter_key *)right;
    size_t common = a->key_length < b->key_length ? a->key_length
                                                  : b->key_length;
    int order = common == 0 ? 0 : memcmp(a->key, b->key, common);
    if (order != 0) return order;
    if (a->key_length == b->key_length) return 0;
    return a->key_length < b->key_length ? -1 : 1;
}

static void iter_collect(tkv_entry *entry, void *user)
{
    struct tinykv_iter *iter = (struct tinykv_iter *)user;
    uint8_t *copy;
    /* The caller holds the db lock discipline; the index must not grow
     * while iterating, which holds because we never put during a walk. */
    if (iter->prefix != NULL &&
        (entry->key_length < iter->prefix_length ||
         memcmp(entry->key, iter->prefix, iter->prefix_length) != 0))
        return;
    copy = (uint8_t *)malloc(entry->key_length ? entry->key_length : 1);
    if (copy == NULL) {
        iter->oom = 1;
        return;
    }
    if (entry->key_length != 0)
        memcpy(copy, entry->key, entry->key_length);
    iter->keys[iter->count].key = copy;
    iter->keys[iter->count].key_length = entry->key_length;
    iter->count++;
}

tinykv_status tinykv_iter_new_prefix(const tinykv_db *db,
                                     const void *prefix,
                                     size_t prefix_length,
                                     tinykv_iter **out_iter)
{
    struct tinykv_iter *iter;
    size_t index_count;
    if (db == NULL || out_iter == NULL) return TINYKV_ERR_INVALID;
    if (prefix == NULL && prefix_length != 0) return TINYKV_ERR_INVALID;
    if (prefix_length > UINT32_MAX) return TINYKV_ERR_INVALID;
    *out_iter = NULL;
    iter = (struct tinykv_iter *)calloc(1, sizeof(*iter));
    if (iter == NULL) return TINYKV_ERR_NOMEM;
    iter->db = (tinykv_db *)db;
    index_count = tkv_index_count(&db->index);
    iter->keys = (struct iter_key *)calloc(
        index_count ? index_count : 1, sizeof(struct iter_key));
    if (iter->keys == NULL) {
        free(iter);
        return TINYKV_ERR_NOMEM;
    }
    if (prefix_length != 0) {
        iter->prefix = (uint8_t *)malloc(prefix_length);
        if (iter->prefix == NULL) {
            tinykv_iter_free(iter);
            return TINYKV_ERR_NOMEM;
        }
        memcpy(iter->prefix, prefix, prefix_length);
        iter->prefix_length = (uint32_t)prefix_length;
    }
    tkv_mutex_lock(handle_mutex(db));
    tkv_index_foreach(&db->index, iter_collect, iter);
    tkv_mutex_unlock(handle_mutex(db));
    if (iter->oom) {
        tinykv_iter_free(iter);
        return TINYKV_ERR_NOMEM;
    }
    qsort(iter->keys, iter->count, sizeof(struct iter_key),
          iter_key_compare);
    *out_iter = iter;
    return TINYKV_OK;
}

tinykv_status tinykv_iter_new(const tinykv_db *db, tinykv_iter **out_iter)
{
    return tinykv_iter_new_prefix(db, NULL, 0, out_iter);
}

tinykv_status tinykv_iter_next(tinykv_iter *iter, const void **out_key,
                               size_t *out_key_length, const void **out_value,
                               size_t *out_value_length)
{
    tinykv_status status;
    if (iter == NULL || out_key == NULL || out_key_length == NULL ||
        out_value == NULL || out_value_length == NULL)
        return TINYKV_ERR_INVALID;

    free(iter->value);
    iter->value = NULL;
    iter->value_length = 0;

    tkv_mutex_lock(&iter->db->mutex);
    while (iter->position < iter->count) {
        struct iter_key *current = &iter->keys[iter->position++];
        void *value = NULL;
        size_t value_length = 0;
        status = tinykv_get(iter->db, current->key, current->key_length,
                            &value, &value_length);
        if (status == TINYKV_ERR_NOT_FOUND)
            continue; /* deleted since the snapshot: skip */
        if (status != TINYKV_OK) {
            tkv_mutex_unlock(&iter->db->mutex);
            return status;
        }
        iter->value = (uint8_t *)value;
        iter->value_length = value_length;
        *out_key = current->key;
        *out_key_length = current->key_length;
        *out_value = iter->value;
        *out_value_length = value_length;
        tkv_mutex_unlock(&iter->db->mutex);
        return TINYKV_OK;
    }
    *out_key = NULL;
    *out_key_length = 0;
    *out_value = NULL;
    *out_value_length = 0;
    tkv_mutex_unlock(&iter->db->mutex);
    return TINYKV_OK;
}

void tinykv_iter_free(tinykv_iter *iter)
{
    size_t i;
    if (iter == NULL) return;
    for (i = 0; i < iter->count; ++i)
        free(iter->keys[i].key);
    free(iter->keys);
    free(iter->value);
    free(iter->prefix);
    free(iter);
}

tinykv_status tinykv_path(const tinykv_db *db, const char **out_path)
{
    if (db == NULL || out_path == NULL) return TINYKV_ERR_INVALID;
    tkv_mutex_lock(handle_mutex(db));
    *out_path = db->path;
    tkv_mutex_unlock(handle_mutex(db));
    return TINYKV_OK;
}

tinykv_status tinykv_file_size(const tinykv_db *db, uint64_t *out_size)
{
    if (db == NULL || out_size == NULL) return TINYKV_ERR_INVALID;
    tkv_mutex_lock(handle_mutex(db));
    *out_size = db->file_size;
    tkv_mutex_unlock(handle_mutex(db));
    return TINYKV_OK;
}

tinykv_status tinykv_count(const tinykv_db *db, uint64_t *out_count)
{
    if (db == NULL || out_count == NULL) return TINYKV_ERR_INVALID;
    tkv_mutex_lock(handle_mutex(db));
    *out_count = (uint64_t)tkv_index_count(&db->index);
    tkv_mutex_unlock(handle_mutex(db));
    return TINYKV_OK;
}

tinykv_status tinykv_close(tinykv_db *db)
{
    if (db == NULL) return TINYKV_OK;
    tkv_mutex_lock(&db->mutex);
    if (db->fd >= 0 && !db->options.read_only) {
        /* A clean close never loses buffered records: hand them to the
         * descriptor before syncing per policy. */
        (void)flush_wbuf(db);
        /* Best-effort maintenance: reclaim when the dead share of the
         * file crosses the configured ratio. Failures keep the close
         * successful; the log stays valid either way. */
        if (db->options.auto_compact_dead_ratio > 0.0) {
            tinykv_stats stats;
            if (tinykv_get_stats(db, &stats) == TINYKV_OK &&
                stats.file_size > 0) {
                double dead_ratio =
                    (double)stats.dead_bytes / (double)stats.file_size;
                if (dead_ratio >= db->options.auto_compact_dead_ratio)
                    (void)tinykv_compact(db);
            }
        }
        if (db->options.sync == TINYKV_SYNC_ON_CLOSE)
            (void)tkv_fsync_file(db->fd);
    }
    tkv_mutex_unlock(&db->mutex);
    if (db->lock_fd >= 0) {
        tkv_unlock(db->lock_fd);
        close(db->lock_fd);
        db->lock_fd = -1;
    }
    if (db->fd >= 0) {
        close(db->fd);
        db->fd = -1;
    }
    db_free(db);
    return TINYKV_OK;
}
