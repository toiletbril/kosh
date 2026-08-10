unset KOSH_FLAGS
# The default mood makes an unmatched glob a hard error, the strict shell default
# that catches a typo, while the bash and sh moods leave it as its literal text
# the way bash and dash do. A matching glob expands the same in every mood.
# The test changes directory, so the relative binary path is resolved first.
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
d=$(mktemp -d); cd "$d" || exit 1
touch real_one real_two
echo "== a matching glob expands to the sorted names:"; "$BIN" -c 'echo real_*'
echo "== the default mood aborts on no match:"; "$BIN" -c 'echo no_match_*'; echo "rc=$?"
echo "== the bash mood leaves it literal:"; "$BIN" --mood bash -c 'echo no_match_*'
echo "== the sh mood leaves it literal:"; "$BIN" --mood sh -c 'echo no_match_*'
cd /; rm -rf "$d"
