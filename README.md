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
