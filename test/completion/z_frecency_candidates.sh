# z completion reads the frecency store, keeps live directories, and ignores
# entries whose directories no longer exist.
d=$(mktemp -d); store=$(mktemp)
mkdir "$d/alpha" "$d/beta"
printf '%s\t10\t9999999999\n%s\t2\t9999999999\n%s\t99\t9999999999\n' \
  "$d/alpha" "$d/beta" "$d/removed" > "$store"
normalized_d=$(printf '%s\n' "$d" | tr '\\' '/')
echo "== recent directories for an empty z query:"
KOSH_DIRECTORY_HISTORY="$store" "$BIN" --debug-complete-at 'z ' </dev/null |
  tr '\\' '/' | sed "s#$normalized_d#TMPDIR#g"
echo "== prefix filters recent directories:"
KOSH_DIRECTORY_HISTORY="$store" "$BIN" --debug-complete-at 'z al' </dev/null |
  tr '\\' '/' | sed "s#$normalized_d#TMPDIR#g"
if [ -n "$d" ] && [ -n "$store" ]; then
  "$TEST_SYSTEM_RM" -r "$d" "$store"
fi
