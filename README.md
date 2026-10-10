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
