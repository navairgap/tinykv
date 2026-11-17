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
test "$("$TOOL" count "$db")" = 2
test "$("$TOOL" list "$db")" = "$(printf 'greeting\thello\nuser\tnaveen')"
"$TOOL" verify "$db"
"$TOOL" compact "$db"
"$TOOL" verify "$db"
"$TOOL" stats "$db" --json > "$work/stats.json"
grep -q '"records": 2' "$work/stats.json"
grep -q '"version": "1.0.0"' "$work/stats.json"
"$TOOL" del "$db" user
if "$TOOL" get "$db" user; then
    echo "smoke: get of a deleted key must fail" >&2
    exit 1
fi
test "$("$TOOL" count "$db")" = 1
printf 'k1\tv1\nk2\tv2\n' | "$TOOL" batch "$db"
test "$("$TOOL" count "$db")" = 3
"$TOOL" copy "$db" "$work/snapshot.db"
test "$("$TOOL" count "$work/snapshot.db")" = 3
test "$("$TOOL" get "$work/snapshot.db" k1)" = v1
if "$TOOL" copy "$db" "$work/snapshot.db"; then
    echo "smoke: copy must refuse an existing destination" >&2
    exit 1
fi
"$TOOL" version
echo "tinykv cli smoke: all checks passed"
