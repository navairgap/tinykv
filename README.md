# tinykv

A small key-value store written in C.

> **Current state: record-codec prototype.** The first append-log record codec is implemented with bounded decoding and focused unit tests. The database, index, WAL replay loop, durability policy, and stable on-disk compatibility guarantees are not implemented yet.

## Record codec

The prototype follows the draft v1 layout in [docs/record-format.md](docs/record-format.md): TKV1 magic, one-byte put/delete type, big-endian key/value lengths, CRC-32/ISO-HDLC checksum, then opaque key and value bytes. The decoder enforces configured size limits before exposing payload pointers and returns a view into the caller-owned input buffer.

Build and run the focused codec tests with a C11 compiler:

    cc -std=c11 -Wall -Wextra -Werror -Iinclude src/record_codec.c tests/test_record_codec.c -o /tmp/tinykv-record-tests
    /tmp/tinykv-record-tests

## Remaining scope

- [ ] Define write, flush, and fsync durability guarantees.
- [ ] Implement append-only log I/O and recovery.
- [ ] Add the in-memory key index.
- [ ] Distinguish a truncated tail from corruption in the middle of a log.
- [ ] Add explicit, recoverable tail-repair behavior.
- [ ] Add compaction only after replay and recovery are stable.

The codec is a prototype, not a database API or a stable storage format. Do not use it as a production persistence layer.

## Non-goals

Networking, replication, distributed consistency, and a production-grade LSM tree remain out of scope for the first version.

## License

MIT. See [LICENSE](LICENSE).
