# storage format review

Before stabilizing an on-disk format, document byte order, record version, key/value lengths, checksum coverage, and durability guarantees. Reject lengths that exceed configured limits before allocating. Define how incomplete final records differ from corruption in the middle of a log. Keep migrations and repair modes explicit. Add fixtures for each format version and verify old data remains readable after compatible changes.
