# tinykv

[![CI](https://github.com/navairgap/tinykv/actions/workflows/ci.yml/badge.svg)](https://github.com/navairgap/tinykv/actions/workflows/ci.yml)

A tiny, crash-safe embedded key-value store in C11. One file, zero
dependencies, append-only storage with CRC-32 checksums, an in-memory
hash index, and explicit recovery and repair semantics — the classic
[Bitcask](https://docs.riak.com/riak/kv/2.2.3/setup/planning/backend/bitcask/index.html)
model, done carefully.

> **Status: v1.5.0.** The on-disk format (`TKV1`), the public API, and the
> durability guarantees below are stable and covered by the test suite,
> sanitizers, ThreadSanitizer, valgrind, and fuzzing.

## Why tinykv

- **Simple.** One append-only log file per database, one hash index in
  memory. No WAL/LSM/B-tree machinery to reason about.
- **Thread-safe by default.** Share one handle across threads; build
  with `NO_THREADS=1` if you serialize externally or have no pthreads.
- **Fast.** Table-driven CRC-32 and a 64 KiB write buffer put replay at
  over a million records per second and puts near 800k ops/s.
- **Crash-safe by construction.** Every record carries a CRC-32/ISO-HDLC
  checksum; a crash can only ever leave a torn tail, which replay
  detects and the next write reclaims. Corruption in the middle of a log
  is *reported*, never silently skipped.
- **Explicit durability.** You choose `TINYKV_SYNC_EVERY_WRITE` (default),
  `TINYKV_SYNC_ON_CLOSE`, or `TINYKV_SYNC_NONE`. File creation, atomic
  compaction, and repair all fsync the containing directory.
- **Non-destructive repair.** `tinykv repair` prints its plan, preserves
  the original bytes in `<db>.bak`, writes and *verifies* the repaired
  file before swapping it in, and refuses to race an open writer.
- **Bounded memory.** Key/value length limits are enforced before any
  allocation, on encode, decode, and open-time replay.

## Quick start (library)

```c
#include "tinykv/db.h"

tinykv_db *db;
tinykv_open("/tmp/demo.tkv", NULL, &db);

tinykv_put(db, "greeting", 8, "hello, world", 12);

void *value;
size_t length;
tinykv_get(db, "greeting", 8, &value, &length);  /* malloc'd copy */
printf("%.*s\n", (int)length, (char *)value);
free(value);

tinykv_compact(db);   /* drop dead bytes atomically */
tinykv_close(db);
```

Prefix iteration filters namespaces without reading other keys:

```c
tinykv_iter *iter;
tinykv_iter_new_prefix(db, "user:", 5, &iter);
/* every "user:*" key, sorted, values re-read on each step */
tinykv_iter_seek(iter, "user:5", 6);   /* jump to any key */
tinykv_iter_free(iter);
```

Batched calls resolve whole arrays under one lock acquisition:

```c
tinykv_get_result reads[16];
tinykv_get_multi(db, reads, 16);   /* per-entry status, malloc'd values */
tinykv_put_multi(db, puts, 16);
tinykv_delete_multi(db, removals, 16);
```

Build and install:

    make all
    make test          # 6 test programs, ~200 assertions
    sudo make install  # libtinykv.a, headers, tinykv tool, pkg-config file

## Quick start (CLI)

    tinykv put demo.tkv greeting hello
    tinykv get demo.tkv greeting          # hello
    tinykv list demo.tkv                  # greeting<TAB>hello
    printf 'a\t1\nb\t2\n' | tinykv batch demo.tkv
    tinykv stats demo.tkv
    tinykv compact demo.tkv
    tinykv verify demo.tkv                # exit 0 clean / 3 torn tail / 4 corrupt
    tinykv repair demo.tkv                # plan only; add --yes to apply

Exit codes distinguish `not found` (2), `torn tail` (3), `corruption`
(4), `locked` (5), and I/O failures (6), so scripts can react precisely.

## How it works

```
put("greeting", "hello")                get("greeting")
      │                                        │
      ▼                                        ▼
┌─────────────────────┐   offset,size    ┌──────────────┐
│ append-only log     │◄─────────────────┤ in-memory    │
│ TKV1 records, CRC32 │                  │ hash index   │
└─────────────────────┘                  └──────────────┘
```

- **Write path:** encode `TKV1` record → append to log → (optional fsync)
  → update index. One sequential write per operation.
- **Read path:** index lookup → one `pread` → checksum verified again →
  value copied out.
- **Open:** replay the log from byte 0, validating every record, building
  the index. A truncated final record is a torn tail (reported, and
  reclaimed before the next write). Anything else that fails to decode is
  corruption: open refuses.
- **Compaction:** copy live records to `<db>.compact`, fsync, atomically
  rename over the log, fsync the directory, then repoint the index.
  Failures leave the original untouched.
- **Repair:** validate → preserve original as `<db>.bak` → write the
  verified prefix to `<db>.repair` → fsync → re-verify → atomic swap.

See [docs/design.md](docs/design.md) for the full architecture and
[docs/durability.md](docs/durability.md) for the fsync ordering rules.

## Performance

`make bench` on one machine (100k keys, 64-byte values, `SYNC_NONE`,
this repo's own harness):

| operation          | ops/s    |
|--------------------|---------:|
| put                | ~260,000 |
| get (malloc)       | ~490,000 |
| get_into (buffer)  | ~529,000 |
| reopen (replay)    | ~1,394,000 records/s |
| delete             | ~340,000 |

With `TINYKV_SYNC_EVERY_WRITE` (the default), each put costs one fsync —
durability you can feel, and exactly what you want when the data matters
more than the microsecond.

## Similar projects

tinykv sits in the same niche as Riak's
[Bitcask](https://docs.riak.com/riak/kv/2.2.3/setup/planning/backend/bitcask/index.html)
backend ([design
writeup](https://highscalability.com/riaks-bitcask-a-log-structured-hash-table-for-fast-keyvalue)),
[LMDB](https://www.symas.com/lmdb) (memory-mapped B+tree, multi-process),
and small educational stores. The differentiators here are the explicit
torn-tail/corruption distinction, the non-destructive repair workflow,
golden-vector checksum fixtures for cross-language interop, and a fully
bounded decoder. When in doubt for a serious project, use LMDB — tinykv
is for when you want that class of behavior in ~1,200 lines of C you can
actually read.

## Documentation

- [docs/tutorial.md](docs/tutorial.md) — start here: the API in five minutes
- [docs/api.md](docs/api.md) — every public function, with semantics
- [docs/design.md](docs/design.md) — architecture and trade-offs
- [docs/durability.md](docs/durability.md) — sync policies and crash cases
- [docs/record-format.md](docs/record-format.md) — the `TKV1` byte format
- [docs/checksum-golden-vectors.md](docs/checksum-golden-vectors.md) — interop fixtures
- [docs/repair-mode-safety.md](docs/repair-mode-safety.md) — repair workflow rules

## Build targets

| target | what it does |
|---|---|
| `make all` | library, tests, tool, example (default) |
| `make test` | run every test program |
| `make smoke` | end-to-end CLI exercise |
| `make fuzz` | deterministic corruption + scanner fuzzing |
| `make bench` | throughput benchmark (keys, value size, threads) |
| `make SAN=1` | address + undefined sanitizer build |
| `make TSAN=1` | ThreadSanitizer build (exclusive with `SAN=1`) |
| `make NO_THREADS=1` | drop pthreads; serialize handles yourself |
| `make install` / `uninstall` | library, headers, tool, pkg-config |
| `make dist` | source tarball of the current tree |

## Testing

Seven test programs cover the codec, index, POSIX I/O layer, log scan
and append, the database lifecycle, checksum fixtures, and concurrency.
CI runs them under gcc and clang, address+undefined sanitizers with
leak detection, ThreadSanitizer, valgrind, a 60-second libFuzzer run of
the codec and scanner, an end-to-end CLI smoke test, and a
`NO_THREADS=1` build. Every mode runs locally too:

    make test           # default -O2 build
    make test SAN=1     # address + undefined sanitizers, leak detection
    make test TSAN=1    # ThreadSanitizer
    make test NO_THREADS=1
    make fuzz           # deterministic corruption + scanner fuzzing
    make smoke          # end-to-end CLI exercise

The fuzz harness includes exhaustive single-byte corruption coverage
and replays every input through the recovery scanner.

## FAQ

**How do I look up many keys at once?**
`tinykv_get_multi` resolves a whole request array under one lock
acquisition, `tinykv_put_multi` and `tinykv_delete_multi` do the same
for writes, and every entry reports its own status. `tinykv_iter_seek`
positions an existing iterator at any key for pagination-style access.

**Can threads share one handle?**
Yes by default — every call takes an internal recursive mutex, so one
handle is safe across threads (serialized, not parallel). Build with
`make NO_THREADS=1` to drop the pthread dependency and serialize
yourself.

**How big can my database be?**
The index holds one entry per live key, so plan for roughly 40–80 bytes
of RAM per key plus the value length you store. The file itself can grow
into the gigabytes; recovery time scales with the log, so compact
regularly on large databases.

**When should I compact?**
`tinykv stats` shows `dead_bytes` — bytes held by superseded records,
tombstones, and torn tails. A rough rule: compact when dead bytes exceed
25–50% of the file, or before taking a backup (compaction makes the copy
smaller and replay faster). Compaction is atomic and safe to run while
your process holds the handle.

**Can two processes use it?**
One process may hold the read-write handle (enforced by an exclusive
flock — a second writer gets `TINYKV_ERR_LOCKED`). Any number of
read-only handles may be open at the same time; each sees the log as of
its open.

**What happens if my process dies mid-write?**
At worst a torn tail: the last, partially-written record. Open reports
it in stats (`discarded_bytes`), the next write reclaims it, and nothing
else is affected. Use `tinykv verify` to check, `tinykv repair --yes` to
reclaim it explicitly.

**Does tinykv give me durability by default?**
Yes — the default `TINYKV_SYNC_EVERY_WRITE` fsyncs after every write.
Bulk loads should switch to `TINYKV_SYNC_ON_CLOSE` or `SYNC_NONE` (the
CLI's `batch --sync=` flag does this), and `tinykv_sync(db)` is the
explicit checkpoint for `SYNC_NONE` writers.

## Non-goals (1.5)

Networking, replication, multi-process writers, ordered range scans
(iteration is a sorted snapshot with prefix filtering, not a B-tree
cursor), compression, and Windows. POSIX only. The format is versioned:
any incompatible change gets a new magic, never a silent
reinterpretation of `TKV1` bytes.

## License

MIT. See [LICENSE](LICENSE).
