# Changelog

All notable changes to tinykv are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
versions [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Features

- `tinykv_exists` presence checks answered from the index alone
- `tinykv_get_multi` batched reads under one lock acquisition
- `tinykv_put_multi` and `tinykv_delete_multi` batched writes
- `tinykv_iter_seek` random-position iteration via binary search
- `file_mode` option for database creation permissions (compaction
  honors it; sidecars stay 0600)
- `tinykv_iter_remaining` upper-bound iteration progress
- CLI `exists`, `count [prefix]`, `verify --json` (with
  `discarded_bytes`), and `get --into=FILE` for binary values
- `batch`/`import` failures name the offending input line
- `tinykv(1)` man page installed by `make install`
- `verify`/`repair` accept `--max-key`/`--max-value` for custom-limit
  databases
- Benchmark phases for `exists` (~3.3M ops/s) and batched reads

### Fixed

- Index capacity is released when a database's index is cleared
- Compaction releases the handle mutex when its temp file cannot be
  created (found by ThreadSanitizer); previously the handle stayed
  locked after a failed compaction on an unwritable directory
- `tkv_copy_file` refuses self-copies instead of truncating the file
- `list` options are fully initialized (uninitialized
  `auto_compact_dead_ratio` field)

### Testing

- Export/import double round-trip property test and binary-value CLI
  output tests
- Compaction failure path (read-only directory), sparse-file hole
  classification, multi-process readers during writes, batched writes
  under threads, iterator seek positioning
- Auto-compact threshold boundary, batch-under-delete, prefix
  iteration after concurrent writes, and iterator progress tests

## [1.5.0] - 2026-11-18

Performance, concurrency, and queryability: the write path triples in
throughput, handles are thread-safe by default, and iteration gains
namespace filtering.

### Features

- **Thread-safe handles by default**: every public call takes an
  internal recursive mutex; build with `-DTINYKV_NO_THREADS` (or
  `make NO_THREADS=1`) to drop the pthread dependency entirely
- **64 KiB write buffer**: appends reach the file one syscall per
  buffer instead of per record; reads, syncs, compaction, and close
  flush first, so clean closes never lose records
- **Table-driven CRC-32**: the checksum loop is a pinned 256-entry
  lookup table — no lazy init, no per-bit loop
- `tinykv_sync` for explicit durable flushes under any sync policy
- Prefix iterators (`tinykv_iter_new_prefix`) and CLI `keys [prefix]` /
  `list [prefix]` namespace filtering
- `auto_compact_dead_ratio` option: compact on close when the dead-byte
  share crosses a threshold (0 disables, the default)
- `index_memory_bytes` in stats and CLI output for capacity planning
- `tinykv export` / `tinykv import` file round-trips sharing batch's
  line parser; `copy` snapshots (1.1 line) unchanged
- Threaded benchmark phases measuring contention honestly

### Performance (make bench, 100k keys, sync off, this machine)

| operation | 1.0 | 1.5 |
|---|---:|---:|
| put | ~167-235k ops/s | ~760-800k ops/s |
| get (malloc) | ~237-346k ops/s | ~360-530k ops/s |
| reopen (replay) | ~392-564k records/s | ~1.04-1.39M records/s |

### Fixed

- Sanitizer flags now propagate to every make invocation (`SAN=1`,
  `TSAN=1`, mutually exclusive)
- The tool detects stdout failures (ENOSPC, closed consumer) and exits 6
- File copies are fsynced before success is reported
- Index capacity doubling guards against size_t overflow
- Uninitialized `auto_compact_dead_ratio` in hand-built option structs
  (tool and tests now set every field explicitly)
- The Makefile tracks header dependencies; touching a header rebuilds
  its dependents

### Testing

- ThreadSanitizer job plus a NO_THREADS build job in CI
- Concurrency smoke: four threads, 8000 interleaved operations, full
  verification of survivors
- Fuzz harness drives the recovery scanner as well as the decoder
- Exhaustive truncation sweep, single-byte corruption sweep, randomized
  model check, cross-process lock proof, iteration-across-compaction

## [1.1.0] - 2026-11-17

Post-1.0 hardening release, folded into 1.5.0 (same-day follow-up to
1.0.0; contents listed under 1.5.0 above).

## [1.0.0] - 2026-11-17

First stable release: a complete, crash-safe embedded key-value store on
the Bitcask model — append-only TKV1 log, in-memory hash index, atomic
compaction, and explicit non-destructive repair.

### Features

- `TKV1` record codec: CRC-32/ISO-HDLC checksummed records with bounded
  decoding that enforces limits before any allocation
- Append-only log with sequential writes and a growing recovery scanner
  that distinguishes a provable torn tail from mid-file corruption
- In-memory open-addressing hash index (FNV-1a, tombstones, 75% growth)
- Database lifecycle: exclusive-flock single writer, read-only handles,
  `create_if_missing`, private 0600 files, deterministic replay
- `put` / `get` / `get_into` / `delete` with re-verified reads and
  `TINYKV_ERR_LIMIT` / `TINYKV_ERR_INVALID` error discipline
- Durability policies: `SYNC_EVERY_WRITE` (default), `SYNC_ON_CLOSE`,
  `SYNC_NONE`; directory fsync on creation and atomic replacement
- Torn-tail reclaim before the first append after a crash
- Atomic compaction with deferred index repointing
- `tinykv_verify` and non-destructive `tinykv_repair` (dry-run, `.bak`
  preservation, verified `.repair`, atomic swap, lock rejection)
- Sorted snapshot iteration that skips concurrently deleted keys
- Stats: records, file/live/dead/discarded bytes, operation counters
- `tinykv` CLI: put/get/del/list/count/stats/compact/verify/repair and
  `batch` stdin import, with scripting-friendly exit codes
- pkg-config file, `make install`/`uninstall`/`dist`

### Testing

- Six test programs covering codec, index, POSIX I/O, log scan,
  database lifecycle, and checksum golden-vector fixtures
- Deterministic fuzz harness with exhaustive single-byte corruption
  coverage plus a libFuzzer entry point for CI
- CI matrix (gcc/clang), address+undefined sanitizers with leak
  detection, valgrind, 60-second libFuzzer smoke, end-to-end CLI smoke

### Performance

- ~235k puts/s, ~350k gets/s, ~564k replayed records/s on the built-in
  benchmark (`make bench`, 100k keys, sync disabled)

[1.5.0]: https://github.com/navairgap/tinykv/releases/tag/v1.5.0
[1.1.0]: https://github.com/navairgap/tinykv/releases/tag/v1.5.0
[1.0.0]: https://github.com/navairgap/tinykv/releases/tag/v1.0.0
