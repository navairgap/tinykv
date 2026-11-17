/* tinykv — a tiny, crash-safe embedded key-value store.
 *
 * Storage model (bitcask-style): an append-only CRC-32-protected log file
 * plus an in-memory hash index. One process opens a database read-write at
 * a time; readers may open it read-only concurrently.
 *
 * The library is not thread-safe: guard a handle with your own locking if
 * multiple threads use it.
 */
#ifndef TINYKV_DB_H
#define TINYKV_DB_H

#include "tinykv/record.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINYKV_VERSION_MAJOR 1
#define TINYKV_VERSION_MINOR 0
#define TINYKV_VERSION_PATCH 0
#define TINYKV_VERSION_STRING "1.0.0"

typedef enum {
    TINYKV_OK = 0,
    TINYKV_ERR_IO,        /* filesystem failure (open/read/write/sync) */
    TINYKV_ERR_NOMEM,     /* allocation failure */
    TINYKV_ERR_INVALID,   /* invalid arguments or options */
    TINYKV_ERR_NOT_FOUND, /* key not present */
    TINYKV_ERR_CORRUPT,   /* log corruption that is not a torn tail */
    TINYKV_ERR_LOCKED,    /* another process holds the write lock */
    TINYKV_ERR_LIMIT,     /* key/value exceeds configured limits */
    TINYKV_ERR_TORN_TAIL  /* a torn tail exists; repair can reclaim it */
} tinykv_status;

typedef enum {
    TINYKV_SYNC_NONE = 0,       /* never fsync: fastest, unsafe on crash */
    TINYKV_SYNC_ON_CLOSE = 1,   /* fsync on close and after compaction */
    TINYKV_SYNC_EVERY_WRITE = 2 /* fsync after every put/delete (default) */
} tinykv_sync_policy;

typedef struct {
    uint32_t max_key_length;   /* default 4096 */
    uint32_t max_value_length; /* default 1048576 */
    tinykv_sync_policy sync;   /* default TINYKV_SYNC_EVERY_WRITE */
    int create_if_missing;     /* default 1 */
    int read_only;             /* default 0 */
    int lock;                  /* exclusive flock on open for writes, default 1 */
} tinykv_options;

typedef struct {
    uint64_t records;         /* live keys in the index */
    uint64_t file_size;       /* log size in bytes */
    uint64_t live_bytes;      /* bytes held by live records */
    uint64_t dead_bytes;      /* tombstones, superseded records, torn tail */
    uint64_t discarded_bytes; /* torn-tail bytes that the next write reclaims */
    uint64_t puts;
    uint64_t deletes;
    uint64_t compactions;
} tinykv_stats;

typedef struct tinykv_db tinykv_db;

/* Open (or create) the database file at path. On failure *out_db stays
 * NULL and a status is returned. options may be NULL for defaults. */
tinykv_status tinykv_open(const char *path, const tinykv_options *options,
                          tinykv_db **out_db);
tinykv_status tinykv_close(tinykv_db *db);

tinykv_status tinykv_put(tinykv_db *db, const void *key, size_t key_length,
                         const void *value, size_t value_length);
tinykv_status tinykv_delete(tinykv_db *db, const void *key,
                            size_t key_length);
/* Returns a malloc'd copy of the value; the caller frees it. */
tinykv_status tinykv_get(const tinykv_db *db, const void *key,
                         size_t key_length, void **out_value,
                         size_t *out_value_length);
/* Copies the value into a caller buffer; TINYKV_ERR_LIMIT reports a
 * capacity shortfall in *out_value_length. */
tinykv_status tinykv_get_into(const tinykv_db *db, const void *key,
                              size_t key_length, void *buffer,
                              size_t capacity, size_t *out_value_length);

/* Rewrite the log keeping only live records, atomically replacing the
 * file. Reclaims dead and discarded bytes. */
tinykv_status tinykv_compact(tinykv_db *db);

tinykv_status tinykv_count(const tinykv_db *db, uint64_t *out_count);
tinykv_status tinykv_get_stats(const tinykv_db *db, tinykv_stats *out_stats);
/* Path the handle was opened with; *out_path is a borrowed pointer
 * valid until close. */
tinykv_status tinykv_path(const tinykv_db *db, const char **out_path);
/* Current log size as tracked by this handle. */
tinykv_status tinykv_file_size(const tinykv_db *db, uint64_t *out_size);

/* Sorted key iteration. The iterator snapshots the key set at creation;
 * values are re-read on each step, and keys deleted meanwhile are skipped.
 * Pointers returned by tinykv_iter_next stay valid until the next call. */
typedef struct tinykv_iter tinykv_iter;
tinykv_status tinykv_iter_new(const tinykv_db *db, tinykv_iter **out_iter);
tinykv_status tinykv_iter_next(tinykv_iter *iter, const void **out_key,
                               size_t *out_key_length, const void **out_value,
                               size_t *out_value_length);
void tinykv_iter_free(tinykv_iter *iter);

/* Validate a log without opening it for writes. *out_last_good_offset
 * reports where replay stopped: TINYKV_OK means a clean end,
 * TINYKV_ERR_TORN_TAIL an incomplete final record, TINYKV_ERR_CORRUPT
 * corruption that must not be skipped. */
tinykv_status tinykv_verify(const char *path, uint32_t max_key_length,
                            uint32_t max_value_length,
                            uint64_t *out_last_good_offset);
/* Explicit, non-destructive repair (see docs/repair-mode-safety.md):
 * validate, write the retained prefix to "<path>.repair", fsync, verify
 * the repaired copy by replay, preserve the original as "<path>.bak",
 * then atomically replace. With dry_run set, nothing is modified and the
 * proposed truncation length is reported. Returns TINYKV_OK when the log
 * is already clean; otherwise the classify status (TINYKV_ERR_TORN_TAIL
 * or TINYKV_ERR_CORRUPT) with *out_last_good_offset as the proposed
 * truncation length. */
tinykv_status tinykv_repair(const char *path, uint32_t max_key_length,
                            uint32_t max_value_length, int dry_run,
                            uint64_t *out_last_good_offset);

const char *tinykv_status_string(tinykv_status status);
const char *tinykv_version_string(void);

#ifdef __cplusplus
}
#endif
#endif
