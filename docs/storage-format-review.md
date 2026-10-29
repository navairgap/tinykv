# storage format review

Before stabilizing an on-disk format, document byte order, record version, key/value lengths, checksum coverage, and durability guarantees. Reject lengths that exceed configured limits before allocating. Define how incomplete final records differ from corruption in the middle of a log. Keep migrations and repair modes explicit. Add fixtures for each format version and verify old data remains readable after compatible changes.

## File safety and backups

Create new database and temporary files with permissions that prevent unrelated local users from reading stored values. Do not follow an unexpected symlink when creating or replacing the database file unless that behavior is an explicit supported option. Document whether backups are consistent snapshots or raw copies, and test restore using a separate temporary directory before relying on a backup for recovery.
