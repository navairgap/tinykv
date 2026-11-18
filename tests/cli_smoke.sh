#!/bin/sh
# End-to-end smoke test for the tinykv tool. Run via: make smoke
set -eu

TOOL=${TOOL:-./build/tinykv}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
db="$work/demo.db"

"$TOOL" put "$db" greeting hello
"$TOOL" put "$db" user naveen
test "$("$TOOL" get "$db" greeting)" = hello
raw=$("$TOOL" get "$db" greeting --raw | od -An -c | tr -d ' \n')
test "$raw" = 'hello' # --raw adds no trailing newline
test "$("$TOOL" count "$db")" = 2
test "$("$TOOL" list "$db")" = "$(printf 'greeting\thello\nuser\tnaveen')"
test "$("$TOOL" keys "$db")" = "$(printf 'greeting\nuser')"
printf 'prefix:one\t1\nprefix:two\t2\n' | "$TOOL" batch "$db" > /dev/null
test "$("$TOOL" keys "$db" 'prefix:')" = "$(printf 'prefix:one\nprefix:two')"
test "$("$TOOL" list "$db" 'prefix:')" = "$(printf 'prefix:one\t1\nprefix:two\t2')"
"$TOOL" del "$db" prefix:one
"$TOOL" del "$db" prefix:two
"$TOOL" verify "$db"
"$TOOL" verify "$db" --json | grep -q '"status": "ok"'
"$TOOL" compact "$db"
"$TOOL" verify "$db"
"$TOOL" stats "$db" --json > "$work/stats.json"
grep -q '"records": 2' "$work/stats.json"
grep -q '"index_memory_bytes"' "$work/stats.json"
grep -q '"version": "1.5.0"' "$work/stats.json"
"$TOOL" exists "$db" greeting
if "$TOOL" exists "$db" missing-key; then
    echo "smoke: exists must exit nonzero for a missing key" >&2
    exit 1
fi
"$TOOL" del "$db" user
if "$TOOL" get "$db" user; then
    echo "smoke: get of a deleted key must fail" >&2
    exit 1
fi
test "$("$TOOL" count "$db")" = 1
printf 'k1\tv1\nk2\tv2\n' | "$TOOL" batch "$db"
test "$("$TOOL" count "$db")" = 3
printf 'synced\t1\n' | "$TOOL" batch "$db" --sync=every
test "$("$TOOL" count "$db")" = 4
if printf 'x\t1\n' | "$TOOL" batch "$db" --sync=bogus; then
    echo "smoke: batch must reject an unknown sync policy" >&2
    exit 1
fi
"$TOOL" export "$db" "$work/dump.tsv"
test "$(wc -l < "$work/dump.tsv")" -ge 3
if "$TOOL" export "$db" "$work/dump.tsv"; then
    echo "smoke: export must refuse an existing file" >&2
    exit 1
fi
"$TOOL" import "$work/fresh.db" "$work/dump.tsv"
test "$("$TOOL" count "$work/fresh.db")" = "$("$TOOL" count "$db")"
test "$("$TOOL" get "$work/fresh.db" greeting)" = hello
"$TOOL" copy "$db" "$work/snapshot.db"
test "$("$TOOL" count "$work/snapshot.db")" = 4
test "$("$TOOL" get "$work/snapshot.db" k1)" = v1
if "$TOOL" export "$db" "$work/dump.tsv"
test "$(wc -l < "$work/dump.tsv")" -ge 3
if "$TOOL" export "$db" "$work/dump.tsv"; then
    echo "smoke: export must refuse an existing file" >&2
    exit 1
fi
"$TOOL" import "$work/fresh.db" "$work/dump.tsv"
test "$("$TOOL" count "$work/fresh.db")" = "$("$TOOL" count "$db")"
test "$("$TOOL" get "$work/fresh.db" greeting)" = hello
"$TOOL" copy "$db" "$work/snapshot.db"; then
    echo "smoke: copy must refuse an existing destination" >&2
    exit 1
fi
"$TOOL" version
echo "tinykv cli smoke: all checks passed"
