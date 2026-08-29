unset KOSH_FLAGS
# z jumps to a directory ranked by the frecency store named by
# KOSH_DIRECTORY_HISTORY, prints the path it picks, errors when nothing matches,
# and skips a stale entry whose directory was removed. The store is seeded by
# hand, path then rank then last-access tab separated, so the ranking is fixed.
d=$(mktemp -d); store=$(mktemp)
normalized_d=$(printf '%s\n' "$d" | tr '\\' '/')
printf '%s\t5\t9999999999\n' "$d" > "$store"
mkdir "$d/start"
echo "== no query changes to HOME:"
(cd "$d/start" && HOME="$d" KOSH_DIRECTORY_HISTORY="$store" "$BIN" -c 'z; pwd') |
    tr '\\' '/' | sed "s#$normalized_d#TMPDIR#"
echo "== a query matches the seeded directory:"
KOSH_DIRECTORY_HISTORY="$store" "$BIN" -c "z $(basename "$d")" |
    tr '\\' '/' | sed "s#$normalized_d#TMPDIR#"
echo "== a query with no match errors:"
KOSH_DIRECTORY_HISTORY="$store" "$BIN" -c 'z no_such_dir_xyz'; echo "rc=$?"
echo "== a higher-ranked but removed entry is skipped:"
printf '%s\t9\t9999999999\n%s\t5\t9999999999\n' "$d/removed" "$d" > "$store"
KOSH_DIRECTORY_HISTORY="$store" "$BIN" -c "z $(basename "$d")" |
    tr '\\' '/' | sed "s#$normalized_d#TMPDIR#"
rm -rf "$d" "$store"
