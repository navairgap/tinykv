# Non-destructive append-log repair (proposed)

Recovery and repair are different operations. Normal database open should report corruption and must not silently rewrite a user's data. Repair should be an explicit operator action.

## Classification

A recovery parser should track the byte offset of the last complete, validated record. A partial final header or payload may be classified as a truncated tail only when the parser can establish that no complete later record exists. A complete record with an invalid magic, type, length, or checksum is corruption, not a harmless truncated tail.

Corruption before the final record boundary must stop automatic recovery. Do not search arbitrary bytes for a plausible next magic sequence; doing so can reinterpret value bytes as records and lose data.

## Repair workflow

1. Open the source read-only and validate as much of the log as possible.
2. Report the last verified byte offset and the exact reason parsing stopped.
3. Require an explicit repair command and a confirmation showing the source path and proposed truncation length.
4. Preserve the original file or create a verified backup before any truncation.
5. Write the repaired result to a new file, flush and sync it, then verify it by reopening and replaying all retained records.
6. Only replace the original after the new file is verified and the operator explicitly approves replacement.

If backup, sync, or verification fails, leave the original untouched. Record the tool version and repair decision in a local report.

## Tests

Use temporary files only. Cover truncated header, truncated payload, invalid checksum in the middle, invalid record length, failed backup, failed sync, interrupted output, and a verified successful repair. No test should modify a real database path.