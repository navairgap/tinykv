# recovery test plan

Recovery tests should cover an empty database, persisted put and delete records, a truncated final record, corruption in the middle of a log, invalid record lengths, a crash between append and index update, deterministic replay, failed writes, and explicit repair mode. Validate record lengths before allocation. Use temporary files in tests and never run destructive recovery tests against a user's real database.
