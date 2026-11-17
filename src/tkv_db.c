#include "tinykv/db.h"
#include "tkv_index.h"
#include "tkv_io.h"
#include "tkv_log.h"
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
    char *path;
    int fd;
    int lock_fd;
    tkv_index index;
    tkv_log_limits limits;
    tinykv_options options;
    uint64_t file_size;
    int torn_tail;           /* a torn tail exists; reclaimed on next write */
    uint64_t tail_last_good; /* offset to truncate to before appending */
    uint64_t puts, deletes, gets, compactions;
};

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
    db->torn_tail = 0;
    return TINYKV_OK;
}

static tinykv_status write_record(tinykv_db *db, uint8_t type,
                                  const void *key, size_t key_length,
                                  const void *value, size_t value_length)
{
    tinykv_status status;
    uint64_t offset;
    size_t record_length;
    int result;

    status = reclaim_torn_tail(db);
    if (status != TINYKV_OK) return status;
    result = tkv_log_append(db->fd, type, key, key_length, value,
                            value_length, &db->limits, &offset,
                            &record_length);
    if (result == -1) return TINYKV_ERR_IO;
    if (result != 0) return status_from_codec((tinykv_record_status)result);
    if (db->options.sync == TINYKV_SYNC_EVERY_WRITE &&
        tkv_fsync_file(db->fd) != 0)
        return TINYKV_ERR_IO;
    if (type == TINYKV_RECORD_PUT) {
        if (tkv_index_put(&db->index, (const uint8_t *)key, key_length,
                          offset, (uint32_t)record_length,
                          (uint32_t)value_length) != 0)
            return TINYKV_ERR_NOMEM;
        db->puts++;
    } else {
        tkv_index_remove(&db->index, (const uint8_t *)key, key_length);
        db->deletes++;
    }
    db->file_size = offset + record_length;
    return TINYKV_OK;
}

tinykv_status tinykv_put(tinykv_db *db, const void *key, size_t key_length,
                         const void *value, size_t value_length)
{
    tinykv_status status =
        write_arguments_status(db, key, key_length, value, value_length);
    if (status != TINYKV_OK) return status;
    if (db->options.read_only) return TINYKV_ERR_INVALID;
    return write_record(db, TINYKV_RECORD_PUT, key, key_length, value,
                        value_length);
}

tinykv_status tinykv_delete(tinykv_db *db, const void *key, size_t key_length)
{
    tinykv_status status =
        write_arguments_status(db, key, key_length, NULL, 0);
    if (status != TINYKV_OK) return status;
    if (db->options.read_only) return TINYKV_ERR_INVALID;
    if (tkv_index_find(&db->index, (const uint8_t *)key, key_length) == NULL)
        return TINYKV_ERR_NOT_FOUND;
    return write_record(db, TINYKV_RECORD_DELETE, key, key_length, NULL, 0);
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

    if (out_value == NULL || out_value_length == NULL)
        return TINYKV_ERR_INVALID;
    *out_value = NULL;
    *out_value_length = 0;
    if (db == NULL || db->fd < 0 || (key == NULL && key_length != 0))
        return TINYKV_ERR_INVALID;

    entry = tkv_index_find(&db->index, (const uint8_t *)key, key_length);
    if (entry == NULL) return TINYKV_ERR_NOT_FOUND;

    record = (uint8_t *)malloc(entry->record_length);
    if (record == NULL) return TINYKV_ERR_NOMEM;
    if (tkv_pread_all(db->fd, record, entry->record_length,
                      entry->offset) != 0) {
        free(record);
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
        return TINYKV_ERR_CORRUPT;
    }
    value = malloc(view.value_length ? view.value_length : 1);
    if (value == NULL) {
        free(record);
        return TINYKV_ERR_NOMEM;
    }
    if (view.value_length != 0)
        memcpy(value, view.value, view.value_length);
    free(record);
    *out_value = value;
    *out_value_length = view.value_length;
    return TINYKV_OK;
}

static void db_free(tinykv_db *db)
{
    if (db == NULL) return;
    tkv_index_free(&db->index);
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

tinykv_status tinykv_count(const tinykv_db *db, uint64_t *out_count)
{
    if (db == NULL || out_count == NULL) return TINYKV_ERR_INVALID;
    *out_count = (uint64_t)tkv_index_count(&db->index);
    return TINYKV_OK;
}

tinykv_status tinykv_close(tinykv_db *db)
{
    if (db == NULL) return TINYKV_OK;
    if (db->fd >= 0 && !db->options.read_only &&
        db->options.sync == TINYKV_SYNC_ON_CLOSE)
        (void)tkv_fsync_file(db->fd);
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
