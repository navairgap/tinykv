# recovery test plan

Recovery tests should cover an empty database, persisted put and delete records, a truncated final record, corruption in the middle of a log, invalid record lengths, a crash between append and index update, deterministic replay, failed writes, and explicit repair mode. Validate record lengths before allocation. Use temporary files in tests and never run destructive recovery tests against a user's real database.

## Durability ordering

For a durable write, document the order in which record bytes, file metadata, and the containing directory become durable. A successful append should not be acknowledged before the configured sync policy is satisfied. Test a failed sync separately from a failed write, and test creation or atomic replacement of a new database file with a directory-sync failure. Keep durability guarantees explicit because a successful userspace write alone does not prove data reached persistent storage.
