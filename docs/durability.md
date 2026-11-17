# Durability and crash behavior

## Sync policies

`tinykv_options.sync` selects when `fsync(2)` reaches the disk:

| policy | behavior | use when |
|---|---|---|
| `TINYKV_SYNC_EVERY_WRITE` (default) | every put/delete fsyncs the log before the call returns | the data must survive power loss |
| `TINYKV_SYNC_ON_CLOSE` | data reaches the OS as writes happen; one fsync at close and after compaction | bulk loads, caches, restartable work |
| `TINYKV_SYNC_NONE` | never fsync | throwaway data, benchmarking |

`TINYKV_SYNC_EVERY_WRITE` is the default because a KV store that
reports success for a write it could lose is lying by omission. Bulk
loading should use `TINYKV_SYNC_ON_CLOSE` (the CLI `batch` command does)
or `TINYKV_SYNC_NONE`.

## Ordering guarantees

A successful `tinykv_put` implies, in order:

1. the record bytes are appended to the log;
2. under `TINYKV_SYNC_EVERY_WRITE`, the file's data is fsynced;
3. the in-memory index points at the new record.

File **creation** (first open that creates the database) and **atomic
replacements** (compaction, repair) additionally fsync the containing
directory, so the file's existence — not just its contents — survives a
crash. A sync failure returns `TINYKV_ERR_IO`: the bytes are in the page
cache and the index, but success was not claimed for the durable layer.

## What a crash can leave behind

Because writes are single appends, a crash or power cut can produce
exactly one anomaly: a **torn tail** — the final record is partially
written. Recovery classifies outcomes:

- **Clean log.** Every record decodes and checksum-verifies. Open
  succeeds; state is exactly the state after the last completed write.
- **Torn tail.** Replay proves the final record is incomplete (it is
  contiguous with the end of file, so nothing after it can exist). Open
  succeeds, `tinykv_stats` reports `discarded_bytes`, and the next write
  or compaction truncates durably to the last verified record before
  appending. No silently corrupted data: the tail never overlaps a
  complete record.
- **Corruption.** Complete bytes that fail magic, type, limits, or CRC.
  This is *not* a torn tail: open returns `TINYKV_ERR_CORRUPT`, and
  repair is an explicit operator action. tinykv never scans ahead for a
  plausible magic — that can reinterpret value bytes as records and eat
  data (see docs/repair-mode-safety.md).

## Failed writes

`tinykv_put` failures are ordered and safe to retry: either the record
was not written (encode/limit/validation failures) or it was written but
not synced (I/O errors) — in which case the index may not include it,
and a retry simply appends a newer version. No failure mode leaves the
index pointing at bytes that are not in the file.

## Repair and backups

Repair follows docs/repair-mode-safety.md: dry-run planning, `<db>.bak`
preservation of the original, a written-and-verified `<db>.repair`, and
an atomic replacement with a directory fsync. Records after the
corruption point are dropped — the `.bak` is the forensic copy. A raw
copy of the database file while no writer holds it is a consistent
snapshot (the log is append-only); take it with the tool stopped or open
the database read-only during the copy.
