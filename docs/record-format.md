# TinyKV append-log record format (draft v1)

This document proposes a simple byte-level format for the first local append-only log. It is not yet an implemented or stable storage format.

## Header

All integers are unsigned big-endian.

| Field | Size | Meaning |
|---|---:|---|
| Magic | 4 bytes | ASCII `TKV1` |
| Record type | 1 byte | `1` = put, `2` = delete |
| Key length | 4 bytes | Number of key bytes |
| Value length | 4 bytes | Number of value bytes; zero for delete |
| Checksum | 4 bytes | CRC-32 over type, lengths, key, and value |

The header is followed by exactly `key_length` bytes and `value_length` bytes. Keys and values are opaque byte strings; the storage layer must not assume UTF-8.

## Checksum interoperability

To keep implementations interoperable, define the checksum as CRC-32/ISO-HDLC (IEEE CRC-32): reflected polynomial `0xEDB88320`, initial value `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. Encode the resulting 32-bit checksum as unsigned big-endian, like the other header integers. The checksum input is the exact concatenation of record type, key length, value length, key bytes, and value bytes; exclude magic and the checksum field itself. Include a fixed golden byte fixture so another implementation can reproduce the same value.

## Validation and recovery

- Reject unknown magic, unsupported record types, impossible lengths, and checksums that do not match.
- Enforce configured maximum key and value sizes before allocating memory.
- A truncated final record may be ignored only if recovery can prove it is the incomplete tail. Corruption in the middle of the log must be reported, not silently skipped.
- Delete records have an empty value and must still include the key.
- Never claim durability until write, flush, and fsync behavior is implemented and documented.

## Versioning

Any incompatible change requires a new magic/version and explicit migration behavior. Keep golden byte fixtures for each supported version.

## Test matrix

Include put/delete round trips, zero-length values, duplicate keys, truncated headers, truncated payloads, bad checksums, oversized lengths, unknown record types, and restart recovery after a partial final write.
