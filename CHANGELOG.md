# Changelog

All notable changes to tinykv are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
versions [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Features

- `tinykv copy` for consistent snapshots into a new file
- `stats --json`, `get --raw`, and `batch --sync=` durability choice

### Fixed

- Sanitizer flags now propagate to every make invocation (`SAN=1`)
- The tool detects stdout failures (ENOSPC, closed consumer) and exits 6
- File copies are fsynced before success is reported
- Index capacity doubling guards against size_t overflow

### Added

- Exhaustive truncation sweep and single-byte corruption sweep tests
- Randomized operations checked against a reference model
- Cross-process lock coverage, `SECURITY.md`, release artifact workflow

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

[1.0.0]: https://github.com/navairgap/tinykv/releases/tag/v1.0.0
