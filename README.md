# tinykv

A tiny, crash-safe embedded key-value store in C11. One file, zero
dependencies, append-only storage with CRC-32 checksums, an in-memory
hash index, and explicit recovery and repair semantics — the classic
[Bitcask](https://docs.riak.com/riak/kv/2.2.3/setup/planning/backend/bitcask/index.html)
model, done carefully.

> **Status: v1.0.0.** The on-disk format (`TKV1`), the public API, and the
> durability guarantees below are stable and covered by the test suite,
> sanitizers, valgrind, and fuzzing.

## Why tinykv

- **Simple.** One append-only log file per database, one hash index in
  memory. No WAL/LSM/B-tree machinery to reason about.
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
| put                | ~235,000 |
| get (malloc)       | ~346,000 |
| get_into (buffer)  | ~351,000 |
| reopen (replay)    | ~564,000 records/s |
| delete             | ~343,000 |

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

- [docs/api.md](docs/api.md) — every public function, with semantics
- [docs/design.md](docs/design.md) — architecture and trade-offs
- [docs/durability.md](docs/durability.md) — sync policies and crash cases
- [docs/record-format.md](docs/record-format.md) — the `TKV1` byte format
- [docs/checksum-golden-vectors.md](docs/checksum-golden-vectors.md) — interop fixtures
- [docs/repair-mode-safety.md](docs/repair-mode-safety.md) — repair workflow rules

## Testing

Six test programs cover the codec, index, POSIX I/O layer, log scan and
append, the database lifecycle, and checksum fixtures. CI runs them
under gcc and clang, address+undefined sanitizers with leak detection,
valgrind, a 60-second libFuzzer run of the codec, and an end-to-end CLI
smoke test. The fuzz harness also runs standalone (`make fuzz`) with
exhaustive single-byte corruption coverage.

## Non-goals (v1.0)

Networking, replication, multi-process writers, concurrent handles in
one process (guard with a mutex if you need it), and Windows. POSIX only.
The format is versioned: any incompatible change gets a new magic, never
a silent reinterpretation of `TKV1` bytes.

## License

MIT. See [LICENSE](LICENSE).
