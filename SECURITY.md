# Security policy

## Reporting

Report suspected vulnerabilities privately via
[GitHub Security Advisories](https://github.com/navairgap/tinykv/security/advisories/new)
("Report a vulnerability"). Please include a reproduction — a test
program or CLI transcript is ideal. Reports are acknowledged within a
week; fixes land in patch releases.

## Scope

tinykv is an embedded library reading one local file that its user
already controls. In scope:

- memory safety of the parser and index (the fuzz harness and
  sanitizers cover this continuously);
- trustworthiness of the read path: a `get` must never return bytes
  that do not verify against the record checksum and key;
- classification correctness: corruption must never be silently
  skipped, and repair must never modify the original before a verified
  replacement exists;
- permission handling: database files are created 0600 and never widen
  existing permissions.

Out of scope for a v1 embedded store: network exposure (tinykv has
none), multi-tenant host defenses beyond file permissions, side-channel
resistance, and SQL/CLI injection (the CLI stores argument bytes
verbatim).

## Hardening notes

- Keys and values are opaque bytes; the decoder enforces configured
  length limits before any allocation, on encode, decode, and replay.
- The CLI expects argument strings; NUL-free binary values should use
  the library API or the TAB-separated `batch` input, never shell
  interpolation.
- Backups via `tinykv copy` inherit the source's content; the copy is
  created 0600 like every database file.
