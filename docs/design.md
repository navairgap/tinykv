# tinykv design

## The model: Bitcask, in a single file

tinykv implements the design Riak popularized as
[Bitcask](https://docs.riak.com/riak/kv/2.2.3/setup/planning/backend/bitcask/index.html)
— see also the [High Scalability
writeup](https://highscalability.com/riaks-bitcask-a-log-structured-hash-table-for-fast-keyvalue)
and Arpit Bhayani's [technical
breakdown](https://arpitbhayani.me/blogs/bitcask):

- all writes append to a log, sequentially, with a checksum;
- an in-memory hash index ("keydir") maps each key to the offset and
  length of its newest record;
- reads are one seek plus one checksum-verified read;
- old versions and tombstones are reclaimed by rewriting only the live
  records (merge/compaction).

Unlike Riak's rotating segment files, tinykv v1 keeps **one log file**
per database and compacts it in place via an atomic rename. That keeps
the recovery story trivial (one file to scan) and the code readable.

## Why not the alternatives?

- **LMDB** is the right answer for multi-process access, huge datasets,
  or when you want copy-on-write B+trees. It is also a different kind of
  thing to read or patch. tinykv targets the "embedded store for one
  process, moderate data, must not lose data" niche with ~1,200 lines of
  dependency-free C11.
- **Full LSM engines** (RocksDB-class) buy range scans and compression
  with enormous complexity. tinykv has no range scans (iteration is a
  sorted key snapshot), which is precisely what keeps the engine small.
- Small educational C KV stores typically skip the two hard parts —
  torn-tail classification and non-destructive repair — which are where
  tinykv spends its care.

## Component map

```
include/tinykv/record.h   TKV1 record codec (public)
include/tinykv/db.h       database API (public)
src/record_codec.c        encode/decode, CRC-32/ISO-HDLC (bitwise)
src/tkv_index.c           open-addressing hash index, FNV-1a, tombstones
src/tkv_io.c              write-all/pread-all loops, fsync file+dir, flock,
                          atomic rename, private 0600 creates
src/tkv_log.c             record append + recovery scan (growing buffer)
src/tkv_db.c              lifecycle, write/read paths, compaction, verify,
                          non-destructive repair, stats, iteration
src/tkv_mutex.h           optional internal handle lock (pthreads or no-op)
src/tkv_tool.c            the tinykv command line tool
```

## Decisions worth knowing

**Checksum before payload trust.** The decoder checks magic, type, and
declared lengths against configured limits *before* reading payload
bytes or allocating anything, then verifies the CRC. A record can never
trick the parser into allocating for a forged 4 GiB length.

**Tail vs corruption.** Recovery parses contiguously from byte 0. If it
stops because a record is incomplete (no bytes remain after it —
provable by construction), the remainder is a torn tail: reported,
ignored at open, truncated before the next append, reclaimable by
repair. If it stops on complete bytes that fail magic/type/limits/CRC,
that is corruption: open refuses, verify reports the offset, repair
requires an explicit operator decision. This follows
docs/repair-mode-safety.md: never scan ahead for a plausible magic, which
can reinterpret value bytes as records.

**Repair writes a new file.** Repair never truncates in place. The
retained prefix is written to `<db>.repair`, fsynced, re-verified by a
full replay, and only then renamed over the original — whose bytes are
preserved as `<db>.bak` first. A failed sync, copy, or verification
leaves everything untouched.

**Index sizing.** Power-of-two open addressing with linear probing,
tombstones for deletes, growth at 75% load (counting tombstones, so
probe chains stay short), FNV-1a 32-bit hashing with the full hash
stored per entry. Keys are owned by the index as private copies.

**Reads re-verify.** Every `get` decodes the record it reads and checks
the CRC and that the stored key equals the requested key. An index bug
or external file tampering cannot return a wrong-but-plausible value.

**Compaction defers index updates.** Live records are copied to
`<db>.compact` with their *future* offsets remembered, not applied. Only
after fsync + rename + directory fsync succeed does the index repoint —
so an interrupted compaction cannot leave the index describing a file
layout that does not exist.

**One writer, many readers.** A read-write open takes a non-blocking
exclusive flock on `<path>.lock`; a second writer gets
`TINYKV_ERR_LOCKED` instead of two logs interleaving. Read-only opens
skip the lock entirely and see a crash-consistent prefix of the log.

**Threads are a compile-time choice.** By default every public call on
a handle takes a recursive internal mutex, so one `tinykv_db` is safe
to share; iteration re-reads values through `tinykv_get` under the same
lock. Embedders who serialize externally build with
`-DTINYKV_NO_THREADS` (or `make NO_THREADS=1`) to drop the pthread
dependency; the mutex compiles to no-ops. A ThreadSanitizer job in CI
exercises the lock, and `NO_THREADS=1` is built and tested there too.

**The write buffer is a durability knob, not a cache.** Appends
accumulate in a 64 KiB userspace buffer handed to the descriptor with
positional writes, so bulk loads pay one syscall per 64 KiB. Reads of
not-yet-flushed records, `tinykv_sync`, compaction, and close all flush
first — a clean close never loses buffered records — and the
`TINYKV_SYNC_*` policies decide when an fsync follows. Only crash
semantics change with `SYNC_NONE`: unflushed bytes vanish exactly like
any unsynced write would.

**Read-side batching stays O(1) per key.** `tinykv_exists` answers
from the index without touching the log, `tinykv_get_multi` takes the
handle lock once for a whole batch, and `tinykv_iter_remaining` is a
subtraction — the 1.5 read-side additions were shaped so callers can
plan work without extra scans.

**Checksums use a pinned table.** The CRC-32/ISO-HDLC lookup table is
constant data in the binary — no lazy init race, no per-bit loop. The
golden-vector tests still verify it against an independent
implementation, which is what makes the speedup trustworthy.

## Explicit non-goals (1.5)

Multi-process writers, network/replication, Windows, compression, and
ordered range scans (iteration is a sorted key snapshot with optional
prefix filtering). Threads inside a handle moved from a v1.0 non-goal
into a supported default; the rest remain scope cuts, not oversights.
