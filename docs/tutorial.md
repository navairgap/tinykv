# tinykv tutorial

A five-minute walkthrough of the whole API, in order. Compile any
snippet against the built library:

    make all
    cc -std=c11 -Iinclude your-program.c build/libtinykv.a -o demo

## 1. Open and close

```c
#include "tinykv/db.h"

tinykv_db *db;
if (tinykv_open("demo.tkv", NULL, &db) != TINYKV_OK) return 1;
/* ... use db ... */
tinykv_close(db);
```

NULL options means the defaults: create if missing, fsync per write,
4096-byte keys, 1 MiB values, one writer enforced by a lock file.

## 2. Put, get, delete

```c
tinykv_put(db, "greeting", 8, "hello", 5);           /* bytes, not strings */
tinykv_put(db, "empty", 5, NULL, 0);                 /* zero-length values */

void *value; size_t length;
if (tinykv_get(db, "greeting", 8, &value, &length) == TINYKV_OK) {
    printf("%.*s\n", (int)length, (char *)value);
    free(value);                                     /* you own the copy */
}

tinykv_delete(db, "greeting", 8);                    /* NOT_FOUND if absent */
```

Every call returns a status; `TINYKV_ERR_NOT_FOUND` and `TINYKV_ERR_LIMIT`
are normal outcomes, not failures to panic about.

## 3. Check presence and read in batches

```c
if (tinykv_exists(db, "empty", 5) == TINYKV_OK) { /* cheap, no read */ }

tinykv_get_result batch[2] = {
    { .key = "empty", .key_length = 5 },
    { .key = "absent", .key_length = 6 },
};
tinykv_get_multi(db, batch, 2);   /* one lock acquisition for both */
/* batch[0].status == TINYKV_OK, batch[0].value is malloc'd (length 0) */
/* batch[1].status == TINYKV_ERR_NOT_FOUND, batch[1].value == NULL */
for (size_t i = 0; i < 2; i++) free(batch[i].value);
```

## 4. Batch reads and writes

```c
tinykv_get_result reads[2] = {
    { .key = "empty",  .key_length = 5 },
    { .key = "absent", .key_length = 6 },
};
tinykv_get_multi(db, reads, 2);      /* one lock acquisition */

tinykv_put_request writes[2] = {
    { .key = "a", .key_length = 1, .value = "1", .value_length = 1 },
    { .key = "b", .key_length = 1, .value = "2", .value_length = 1 },
};
tinykv_put_multi(db, writes, 2);

tinykv_key_result removals[1] = {
    { .key = "a", .key_length = 1 },
};
tinykv_delete_multi(db, removals, 1);  /* NOT_FOUND if already gone */
```

Every entry carries its own status; free the malloc'd values from
successful reads.

## 5. Iterate

```c
tinykv_iter *iter;
tinykv_iter_new_prefix(db, "user:", 5, &iter);   /* NULL, 0 = whole keyspace */
const void *key, *value; size_t klen, vlen;
while (tinykv_iter_next(iter, &key, &klen, &value, &vlen) == TINYKV_OK &&
       key != NULL)
    printf("%.*s = %.*s\n", (int)klen, (const char *)key,
           (int)vlen, (const char *)value);
/* Jump to any key (binary search over the snapshot): */
tinykv_iter_seek(iter, "user:5", 6);
tinykv_iter_free(iter);
```

The iterator snapshots the key set, sorted; values are re-read on each
step, and keys deleted meanwhile are skipped.

## 6. Maintain

```c
tinykv_stats stats;
tinykv_get_stats(db, &stats);
printf("records=%llu dead=%llu index=%llu\n",
       (unsigned long long)stats.records,
       (unsigned long long)stats.dead_bytes,
       (unsigned long long)stats.index_memory_bytes);

tinykv_sync(db);     /* durability checkpoint under any sync policy */
tinykv_compact(db);  /* drop dead bytes atomically */
```

Or set `options.auto_compact_dead_ratio = 0.5` at open and let close do
it when the log is more than half dead.

## 7. Durability choices

```c
tinykv_options options = {0};   /* zero, then set what you need */
options.max_key_length = 1024;
options.max_value_length = 4096;
options.sync = TINYKV_SYNC_ON_CLOSE;   /* fast bulk load, synced at close */
tinykv_open("bulk.tkv", &options, &db);
/* ... many puts ... */
tinykv_close(db);
```

Set every field you care about by hand — the zero value disables the
opt-in features (auto-compaction, no locking). For per-write fsyncs use
the default; for the fastest possible load use `TINYKV_SYNC_NONE` and
call `tinykv_sync` at checkpoints. See docs/durability.md for the crash
semantics of each choice.

## 8. The CLI does the same things

    tinykv put demo.tkv greeting hello
    tinykv get demo.tkv greeting --raw
    tinykv exists demo.tkv greeting && echo present
    tinykv list demo.tkv 'prefix:'       # filtered key/value dump
    tinykv export demo.tkv backup.tsv    # snapshot as TSV
    tinykv import fresh.tkv backup.tsv   # restore into a new database
    tinykv verify demo.tkv               # exit 0 clean / 3 torn / 4 corrupt
    tinykv repair demo.tkv --yes         # reclaim a torn tail or corruption
    tinykv stats demo.tkv --json
