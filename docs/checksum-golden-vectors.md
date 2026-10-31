# Checksum interoperability golden vectors (proposed)

TinyKV's proposed record format uses CRC-32/ISO-HDLC over the record type, lengths, key bytes, and value bytes. Implementations need fixed vectors so the checksum definition is testable across languages.

## Algorithm identifier

Use CRC-32/ISO-HDLC (commonly called IEEE CRC-32): reflected polynomial 0xEDB88320, initial value 0xFFFFFFFF, and final XOR 0xFFFFFFFF. The four-byte checksum field is encoded as unsigned big-endian. The magic and checksum field itself are excluded from the checksum input.

## Independent algorithm vector

The ASCII byte sequence 123456789 has CRC-32/ISO-HDLC value 0xCBF43926, encoded as bytes CB F4 39 26. This well-known algorithm vector verifies the CRC primitive, but it does not by itself verify TinyKV's record-field ordering.

## Record-level fixture procedure

For each record fixture, store:
- the complete record bytes, including magic and encoded checksum;
- the exact checksum-input bytes as a separate fixture field;
- the expected checksum as eight hexadecimal digits;
- a short description of record type, key, and value.

Generate the expected value with an independent CRC-32/ISO-HDLC implementation, then verify it with the implementation under test. Do not calculate the expected checksum using the same helper that the parser calls, because that would make the test circular.

Include put records with empty and non-empty values, delete records with empty values, non-ASCII opaque key/value bytes, and the smallest valid key. Keep fixtures small and deterministic.

## Validation cases

Tests should reject a one-bit change to the checksum input, a byte-order reversal of the stored checksum, a checksum computed over the magic field, and a checksum that accidentally includes the checksum field. The on-disk format remains a proposal until implementation and fixture tests exist.