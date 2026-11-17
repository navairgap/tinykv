# tinykv API reference (v1.0.0)

Header: `#include "tinykv/db.h"`. Link against `libtinykv.a`.
Every function returns `tinykv_status`; `TINYKV_OK` means success. Keys
and values are opaque byte strings — NUL bytes and high bytes are fine.
Unless stated otherwise, passing NULL where a value is required returns
`TINYKV_ERR_INVALID` and leaves outputs untouched.

## Status codes

| status | meaning |
|---|---|
| `TINYKV_OK` | success |
| `TINYKV_ERR_IO` | filesystem failure (open, read, write, fsync, rename) |
| `TINYKV_ERR_NOMEM` | allocation failure |
| `TINYKV_ERR_INVALID` | invalid arguments or options |
| `TINYKV_ERR_NOT_FOUND` | key not present |
| `TINYKV_ERR_CORRUPT` | corruption that is not a torn tail |
| `TINYKV_ERR_LOCKED` | another process holds the write lock |
| `TINYKV_ERR_LIMIT` | key/value exceeds configured limits |
| `TINYKV_ERR_TORN_TAIL` | incomplete final record (repair can reclaim) |

## Lifecycle

### `tinykv_open`
```c
tinykv_status tinykv_open(const char *path, const tinykv_options *options,
                          tinykv_db **out_db);
```
Opens or creates the database file at `path`. On failure `*out_db` stays
NULL. Options (NULL = defaults):

| field | default | meaning |
|---|---|---|
| `max_key_length` | 4096 | reject longer keys with `TINYKV_ERR_LIMIT` |
| `max_value_length` | 1048576 | reject longer values likewise |
| `sync` | `TINYKV_SYNC_EVERY_WRITE` | durability policy (see durability.md) |
| `create_if_missing` | 1 | 0 = fail instead of creating |
| `read_only` | 0 | read-only handles never lock or write |
| `lock` | 1 | exclusive flock via `<path>.lock` |

Open replays the whole log. A torn tail is tolerated (reclaimed on the
next write); mid-log corruption returns `TINYKV_ERR_CORRUPT` — run
`tinykv_verify`/`tinykv repair` in that case.

### `tinykv_close`
```c
tinykv_status tinykv_close(tinykv_db *db);
```
Flushes per the sync policy (`TINYKV_SYNC_ON_CLOSE` fsyncs here),
releases the lock, frees everything. NULL is a no-op returning
`TINYKV_OK`.

## Data

### `tinykv_put`
```c
tinykv_status tinykv_put(tinykv_db *db, const void *key, size_t key_length,
                         const void *value, size_t value_length);
```
Appends a PUT record, syncs per policy, then updates the index.
`key == NULL` is legal only with `key_length == 0`; same for value.
Oversized inputs return `TINYKV_ERR_LIMIT` without touching the file.

### `tinykv_get`
```c
tinykv_status tinykv_get(const tinykv_db *db, const void *key,
                         size_t key_length, void **out_value,
                         size_t *out_value_length);
```
Returns a malloc'd copy of the value; the caller frees it. Zero-length
values yield a non-NULL pointer with length 0. The record is re-verified
(checksum + key match) on every read.

### `tinykv_get_into`
```c
tinykv_status tinykv_get_into(const tinykv_db *db, const void *key,
                              size_t key_length, void *buffer,
                              size_t capacity, size_t *out_value_length);
```
Copies into a caller buffer, no allocation. `*out_value_length` is
always set to the value's length; a short buffer returns
`TINYKV_ERR_LIMIT` with the required size. Zero-length values need no
buffer (`buffer == NULL, capacity == 0` is valid).

### `tinykv_delete`
```c
tinykv_status tinykv_delete(tinykv_db *db, const void *key,
                            size_t key_length);
```
Appends a tombstone and removes the key from the index. Deleting a key
that is not present returns `TINYKV_ERR_NOT_FOUND` and writes nothing.

## Maintenance

### `tinykv_compact`
```c
tinykv_status tinykv_compact(tinykv_db *db);
```
Rewrites the log keeping only live records. Atomic: the compacted file
is fsynced, renamed over the log, and the directory is synced before the
index is repointed. Any failure leaves the original untouched. Also
reclaims a torn tail.

### `tinykv_verify`
```c
tinykv_status tinykv_verify(const char *path, uint32_t max_key_length,
                            uint32_t max_value_length,
                            uint64_t *out_last_good_offset);
```
Read-only classification of a log: `TINYKV_OK` (clean),
`TINYKV_ERR_TORN_TAIL`, or `TINYKV_ERR_CORRUPT`. The last fully verified
offset is reported either way.

### `tinykv_repair`
```c
tinykv_status tinykv_repair(const char *path, uint32_t max_key_length,
                            uint32_t max_value_length, int dry_run,
                            uint64_t *out_last_good_offset);
```
Non-destructive repair per docs/repair-mode-safety.md: refuses while a
writer holds the lock; with `dry_run` reports only; otherwise backs the
original up to `<path>.bak`, writes the verified prefix to
`<path>.repair`, fsyncs it, re-verifies by replay, and atomically
replaces. A clean log is a no-op. Repair *drops* records after the
corruption point — that is its documented behavior, and `<path>.bak`
preserves the originals.

## Introspection

### `tinykv_count`, `tinykv_get_stats`
```c
tinykv_status tinykv_count(const tinykv_db *db, uint64_t *out_count);
tinykv_status tinykv_get_stats(const tinykv_db *db, tinykv_stats *out);
```
`tinykv_stats` reports live records, file size, live/dead/discarded
bytes, and put/delete/compaction counters. `dead_bytes` is space
reclaimable by compaction (superseded records, tombstones, torn tails).

### `tinykv_iter_new` / `tinykv_iter_next` / `tinykv_iter_free`
```c
tinykv_status tinykv_iter_new(const tinykv_db *db, tinykv_iter **out);
tinykv_status tinykv_iter_next(tinykv_iter *iter, const void **out_key,
                               size_t *out_key_length,
                               const void **out_value,
                               size_t *out_value_length);
void tinykv_iter_free(tinykv_iter *iter);
```
Snapshot of the live key set at creation, visited in lexicographic
order. Values are re-read at each step, so keys deleted after the
snapshot are skipped and values reflect the newest writes. Pointers stay
valid until the next step; iteration ends with `*out_key == NULL`.

## Concurrency

One handle is not thread-safe; serialize access yourself. Across
processes, one read-write handle is enforced by an exclusive flock on
`<path>.lock`; read-only handles never take the lock.

## Record codec

`tinykv/record.h` additionally exposes the standalone record codec
(`tinykv_record_encode`, `tinykv_record_decode`, `tinykv_crc32_iso_hdlc`,
`tinykv_record_status_string`) for tools that need the TKV1 format
directly. See docs/record-format.md for the byte layout and
docs/checksum-golden-vectors.md for interop fixtures.
