# tinykv

A small key-value store in C with an LRU cache

> 🚧 **Status: planning** — architecture and README first, code next.

## Why

You don't understand databases until you've written one. tinykv is a single-file-ish C project: append-only log, in-memory index, compaction, and an LRU cache — then benchmarked against reality.

## Planned features

- get/set/delete with an append-only log for durability
- In-memory hash index with CRC-checked entries
- Log compaction (merging old versions)
- LRU cache layer in front of the log
- A tiny benchmark suite and a REPL client

## Stack

`c` `make` `posix-threads`

## Notes

Grow in stages: hashtable → log → compaction → cache. One stage per commit.

## License

MIT, see [LICENSE](LICENSE).

---
maintained · verified 2026-09-30

## Design decisions

- LSM-tree with leveled compaction — writes never block reads.
- Checksums on every block; a torn tail is truncated on open, not fatal.
- No network layer by design. Embed it, don't expose it.

## Embedding

```rust
use tinykv::Db;

let db = Db::open("./data")?;
db.insert(b"greeting", b"hello")?;
assert_eq!(db.get(b"greeting")?, Some(b"hello".to_vec()));
```

That's the whole API surface for 90% of uses. Migrations and TTL live behind feature flags.

## Glossary

- **lsm-tree** — writes go to memory, sorted runs flush to disk
- **wal** — write-ahead log; the crash-recovery backbone
- **compaction** — merging sorted runs to reclaim space and drop shadowed keys

## Glossary

- **lsm-tree** — writes go to memory, sorted runs flush to disk
- **wal** — write-ahead log; the crash-recovery backbone
- **compaction** — merging sorted runs to reclaim space and drop shadowed keys

## Crash recovery

open → replay wal → verify block checksums → truncate torn tail → build index. recovery is linear in wal size, not data size — rotate the wal often.

## Internals

in-memory index is a skiplist — ordered scans without rebalancing pain. levels on disk are sorted runs merged by size-tiered compaction; reads check newest to oldest.

## Compatibility

the on-disk format is versioned in the manifest; opening an older version refuses cleanly instead of guessing. format stability is promised from v1.0 — before that, `tinykv migrate` exists.
