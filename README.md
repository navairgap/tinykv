# tinykv

A planned embedded key-value store written in C.

> **Status: design stage.** This repository currently contains documentation only; there is no database implementation or stable on-disk format yet.

## Goal

Explore the fundamentals of a small local key-value store: a write-ahead log, an in-memory index, crash recovery, and eventually compaction.

## Proposed first version

- put, get, and delete operations for byte strings.
- Append-only persistence with a documented record format.
- Recovery that detects an incomplete final record.
- A small CLI or REPL for manual testing.
- No network server; callers should embed the library rather than expose it directly.

## Correctness and data safety

- Define durability guarantees before implementation (including whether put calls fsync).
- Validate record lengths before allocating memory.
- Detect corrupt records and report errors rather than silently accepting data.
- Keep file-format versioning explicit.
- Test truncated writes, corrupted checksums, duplicate keys, deletion, and restart recovery.
- Do not claim transactional semantics or crash safety until those tests exist.

## Planned milestones

- [ ] Specify the on-disk record format.
- [ ] Implement the append-only log and recovery parser.
- [ ] Add an in-memory index and deterministic unit tests.
- [ ] Add compaction only after recovery behavior is stable.
- [ ] Benchmark using reproducible workloads.

## Non-goals for the first version

Networking, replication, distributed consistency, and a production-grade LSM tree are out of scope until the single-node storage path is correct.

## License

MIT. See [LICENSE](LICENSE).


## Storage safety

Document record lengths, byte order, checksum coverage, and durability guarantees before stabilizing the file format. Validate lengths before allocation and distinguish a truncated final record from corruption in the middle of the log. Test recovery using temporary files.


## Maintenance and verification notes

Add storage review notes covering record lengths, byte order, corruption versus truncated tail behavior, explicit repair mode, and recovery tests using temporary files.
