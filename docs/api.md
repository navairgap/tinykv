# tinykv API reference (v1.5.0)

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
| `auto_compact_dead_ratio` | 0.0 | compact on close when dead bytes reach this share (0.0-1.0; 0 disables) |
| `file_mode` | 0600 | database file permissions on creation; owner r/w required, no world-write; sidecars stay 0600 |

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

### `tinykv_exists`
```c
tinykv_status tinykv_exists(const tinykv_db *db, const void *key,
                            size_t key_length);
```
`TINYKV_OK` when the key is live, `TINYKV_ERR_NOT_FOUND` when it is
not — answered from the index without reading, verifying, or copying
any value.

### `tinykv_get_multi`
```c
tinykv_status tinykv_get_multi(tinykv_db *db, tinykv_get_result *requests,
                               size_t count);
```
Resolves many keys under one lock acquisition. Each entry's
`status`/`value`/`value_length` are filled per key; `value` is malloc'd
only when that entry's status is `TINYKV_OK` (zero-length values are
still non-NULL) and the caller frees it. The overall status is
`TINYKV_OK` only if every entry succeeded; otherwise it is the first
failing entry's status. Entries are evaluated at execution time —
deletes after the request list was built are visible to the batch.

### `tinykv_put_multi`, `tinykv_delete_multi`
```c
typedef struct {
    const void *key;
    size_t key_length;
    const void *value;
    size_t value_length;
    tinykv_status status;
} tinykv_put_request;

typedef struct {
    const void *key;
    size_t key_length;
    tinykv_status status;
} tinykv_key_result;

tinykv_status tinykv_put_multi(tinykv_db *db, tinykv_put_request *requests,
                               size_t count);
tinykv_status tinykv_delete_multi(tinykv_db *db, tinykv_key_result *requests,
                                  size_t count);
```
Whole request arrays under one lock acquisition, mirroring
`tinykv_get_multi`: per-entry statuses, overall status of the first
failure (including skipped invalid entries and read-only handles).
Deletes report `TINYKV_ERR_NOT_FOUND` for absent keys without writing
tombstones. Durability follows the handle's sync policy per record.

### `tinykv_delete`
```c
tinykv_status tinykv_delete(tinykv_db *db, const void *key,
                            size_t key_length);
```
Appends a tombstone and removes the key from the index. Deleting a key
that is not present returns `TINYKV_ERR_NOT_FOUND` and writes nothing.

## Maintenance

### `tinykv_sync`
```c
tinykv_status tinykv_sync(tinykv_db *db);
```
Flushes buffered records and fsyncs the log now, under any sync policy —
the durability point for `TINYKV_SYNC_NONE` writers. Read-only handles
return `TINYKV_ERR_INVALID`.

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
bytes, put/delete/compaction counters, and `index_memory_bytes` — an
approximate resident index size (bucket array plus one allocation per
live key; malloc overhead not modeled). `dead_bytes` is space
reclaimable by compaction (superseded records, tombstones, torn tails).

### `tinykv_iter_new` / `tinykv_iter_next` / `tinykv_iter_free`
```c
tinykv_status tinykv_iter_new(const tinykv_db *db, tinykv_iter **out);
tinykv_status tinykv_iter_new_prefix(const tinykv_db *db, const void *prefix,
                                     size_t prefix_length,
                                     tinykv_iter **out);
tinykv_status tinykv_iter_next(tinykv_iter *iter, const void **out_key,
                               size_t *out_key_length,
                               const void **out_value,
                               size_t *out_value_length);
void tinykv_iter_free(tinykv_iter *iter);
```
Snapshot of the live key set at creation, visited in lexicographic
order. The prefix variant snapshots only keys starting with the prefix
bytes (NULL prefix, length 0 = whole keyspace). Values are re-read at
each step, so keys deleted after the
snapshot are skipped and values reflect the newest writes. Pointers stay
valid until the next step; iteration ends with `*out_key == NULL`. `tinykv_iter_seek` repositions
the iterator to the first snapshot key at or after the given bytes
(binary search; the prefix filter still applies; seeking backwards
restarts an earlier pass). `tinykv_iter_remaining`
reports an upper bound on the keys still to be produced (snapshot size
minus attempted steps; mid-iteration deletes can lower the real yield).

## Concurrency

One handle may be shared across threads: every public call takes an
internal recursive mutex. Embedders who serialize externally can build
with `-DTINYKV_NO_THREADS` (or `make NO_THREADS=1`) to drop the pthread
dependency. Across processes, one read-write handle is enforced by an
exclusive flock on `<path>.lock`; read-only handles never take the
lock.

## Record codec

`tinykv/record.h` additionally exposes the standalone record codec
(`tinykv_record_encode`, `tinykv_record_decode`, `tinykv_crc32_iso_hdlc`,
`tinykv_record_status_string`) for tools that need the TKV1 format
directly. See docs/record-format.md for the byte layout and
docs/checksum-golden-vectors.md for interop fixtures.
