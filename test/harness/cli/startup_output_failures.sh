directory=$(mktemp -d) || exit 1
trap 'cd / && [ -n "$directory" ] && "$BIN_DIR/invoke-koshkit" rm -rf -- "$directory"' EXIT

# --format and --version report a failed write and exit 1 instead of aborting,
# a for loop over a readonly name reports the assignment in the kosh mood
# instead of aborting while it restores the name, and a z store past its 500
# entry cap shrinks to the cap on the next recorded change, keeping the
# directory just visited.

printf 'echo hi\n' > "$directory/script.sh"
if [ -c /dev/full ] && [ "${OS-}" != Windows_NT ]; then
  "$BIN" --format "$directory/script.sh" > /dev/full 2> /dev/null
  printf 'format-full-device=%s\n' "$?"
  "$BIN" --version > /dev/full 2> /dev/null
  printf 'version-full-device=%s\n' "$?"
else
  printf 'format-full-device=1\nversion-full-device=1\n'
fi
"$BIN" --no-diagnostics -c 'readonly x=1; for x in a; do :; done; echo unreachable' 2> /dev/null
printf 'readonly-loop=%s\n' "$?"
"$BIN" --no-diagnostics -M bash -c 'readonly x=1; for x in a; do :; done; echo "bash-mood-continues x=$x"' 2> /dev/null

store=$directory/store
mkdir "$directory/visited"
"$BIN" -c 'for ((i = 0; i < 2000; i++)); do printf "%s/gone%s\t%s\t%s\n" "$1" "$i" "$((i % 7 + 1))" 1; done' _ "$directory" > "$store"
KOSH_DIRECTORY_HISTORY=$store "$BIN" -c "z '$directory/visited'" > /dev/null
printf 'store-lines=%s\n' "$("$BIN_DIR/invoke-koshkit" wc -l < "$store" | "$BIN_DIR/invoke-koshkit" tr -d ' ')"
case $(cat "$store") in
*"visited	"*) printf 'store-keeps-visited=yes\n' ;;
*) printf 'store-keeps-visited=no\n' ;;
esac
