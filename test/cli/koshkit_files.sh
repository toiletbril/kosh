# The koshkit file utilities run inside a fresh temporary directory so the output
# names no absolute path and stays the same on every machine. The binary path is
# resolved to an absolute one first, since the working directory changes.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1

"$BIN" -c 'koshkit mkdir -p a/b'
"$BIN" -c 'koshkit seq 3 > nums.txt'
echo "--- ls ---"
"$BIN" -c 'koshkit ls'
echo "--- ls a ---"
"$BIN" -c 'koshkit ls a'
"$BIN" -c 'koshkit cp nums.txt copy.txt'
"$BIN" -c 'koshkit mv copy.txt moved.txt'
"$BIN" -c 'koshkit touch stamp'
"$BIN" -c 'koshkit ln -s nums.txt sym'
# The owner, the group, and the time of a long row vary by machine, so only the
# mode, the link count, the size, and the name are kept for a stable golden.
echo "--- ls -l sym (mode nlink size name) ---"
# Linux always reports a symlink as mode 0777, while macOS stores the real
# symlink permissions, so the meaningless symlink mode field is folded to the
# Linux form before the row is trimmed.
"$BIN" -c 'koshkit ls -l sym' | sed 's/^l[rwxsStT-]\{9\}/lrwxrwxrwx/' | awk '{print $1, $2, $5, $NF}'
echo "--- ls after operations ---"
"$BIN" -c 'koshkit ls'
echo "--- du -s nums.txt ---"
"$BIN" -c 'koshkit du -s nums.txt'
echo "--- basename ---"
"$BIN" -c 'koshkit basename /usr/local/libfoo.so .so'
echo "--- dirname ---"
"$BIN" -c 'koshkit dirname /usr/local/libfoo.so'
echo "--- realpath lexical basename ---"
"$BIN" -c 'koshkit mkdir -p usr/lib; resolved=$(koshkit realpath ./usr/../usr/lib) || exit; koshkit basename "$resolved"'
"$BIN" -c 'koshkit rmdir a/b'
echo "--- ls a after rmdir ---"
"$BIN" -c 'koshkit ls a'
echo "--- unlink removes a single file ---"
"$BIN" -c 'koshkit touch victim; koshkit unlink victim; echo "unlink rc=$?"'
"$BIN" -c 'koshkit ls' | grep -c victim
echo "--- unlink a directory fails ---"
"$BIN" -c 'koshkit unlink a' 2>&1
echo "rc=$?"
