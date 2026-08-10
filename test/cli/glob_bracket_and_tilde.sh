unset KOSH_FLAGS
# A bracket with no closing bracket is not a glob in the bash mood, so it stays
# literal with no directory scan. The ~+ prefix expands to PWD the way bash reads
# it.
# The test changes directory, so the relative binary path is resolved first.
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
start=$PWD
d=$(mktemp -d); cd "$d" || exit 1
# macOS resolves the temp directory under a /private prefix, so the physical PWD
# is substituted as well as the unresolved mktemp path.
real_d=$(pwd -P)
echo "== an unclosed bracket stays literal in the bash mood:"; "$BIN" --mood bash -c 'echo a[b'
echo "== tilde plus expands to PWD:"; "$BIN" -c 'test ~+ = "$PWD" && echo TMPDIR'
cd "$start" || exit 1
rm -rf "$d"
