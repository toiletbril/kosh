# shellcheck disable=SC2154
unset KOSH_FLAGS
# caller returns 1 at the top level and prints the frame line and source inside
# a function. A non-numeric operand is a located error pointing at the operand.
src=$TEST_TEMP_DIRECTORY/caller-source
"$BIN" -c 'koshkit mkdir -p "$1"' setup "$TEST_TEMP_DIRECTORY"
printf '%s\n' 'f() {' '  caller 0' '}' 'f' > "$src"

echo "== caller at top level returns 1:"
"$BIN" -c 'caller'; echo "rc=$?"
echo "== caller 0 inside a function prints line and source:"
"$BIN" -c ". $src" 2>&1 | sed "s|$src|SRC|"
echo "== caller with a non-numeric operand is a located error:"
"$BIN" -c 'caller abc' 2>&1; echo "rc=$?"
"$BIN" -c 'koshkit unlink "$1"' cleanup "$src"
