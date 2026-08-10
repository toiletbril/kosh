#!/bin/sh

directory=$(mktemp -d)
trap '[ -n "$directory" ] && /bin/rm -rf "$directory"' EXIT

for utility in cat head sort tail uniq wc; do
    "$BIN" -c "koshkit $utility '$directory'" >/dev/null 2>&1
    printf '%s=%s\n' "$utility" "$?"
done

READ_DIRECTORY=$directory "$BIN" -c 'source "$READ_DIRECTORY"' \
    >/dev/null 2>&1
printf 'source=%s\n' "$?"
