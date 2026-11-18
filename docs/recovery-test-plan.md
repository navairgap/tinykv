# recovery test plan

This plan is implemented: tests/test_log.c and tests/test_db.c cover
every case below. Recovery tests cover an empty database, persisted put and delete records, a truncated final record, corruption in the middle of a log, invalid record lengths, a crash between append and index update, deterministic replay, failed writes, and explicit repair mode. Validate record lengths before allocation. Use temporary files in tests and never run destructive recovery tests against a user's real database.

## Durability ordering

For a durable write, the order is: record bytes reach the descriptor (through the 64 KiB write buffer), then fsync per the configured policy, then the containing directory is synced on creation and atomic replacement. A successful append is not acknowledged before the configured sync policy is satisfied; failed syncs return TINYKV_ERR_IO separately from failed writes. See docs/durability.md for the full contract and crash-case classification.
