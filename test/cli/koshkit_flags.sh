# The human-readable -h on du and ls, the verbose -v on cp and mv, and the
# located error a utility renders in the bash mood the same as in the default
# mood. The input file has a fixed size so the human form is the same on every
# machine.
unset KOSH_FLAGS
# A fixed umask keeps the rendered file mode the same on every machine.
umask 022
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1
"$BIN" -c 'koshkit seq 1 500 > big.txt'

echo "--- du -s prints bytes ---"
"$BIN" -c 'koshkit du -s big.txt'
echo "--- du -sh is human-readable ---"
"$BIN" -c 'koshkit du -sh big.txt'
# The owner, the group, and the time vary by machine, so the golden keeps only
# the mode, the link count, the size, and the name of the long row.
echo "--- ls -l prints bytes ---"
"$BIN" -c 'koshkit ls -l big.txt' | sed 's/^-rw-rw-rw- /-rw-r--r-- /' | awk '{print $1, $2, $5, $NF}'
echo "--- ls -lh is human-readable ---"
"$BIN" -c 'koshkit ls -lh big.txt' | sed 's/^-rw-rw-rw- /-rw-r--r-- /' | awk '{print $1, $2, $5, $NF}'
echo "--- cp -v names the copy ---"
"$BIN" -c 'koshkit cp -v big.txt copy.txt'
echo "--- mv -v names the move ---"
"$BIN" -c 'koshkit mv -v copy.txt moved.txt'
echo "--- utility error is located in the bash mood ---"
"$BIN" --mood bash -c 'koshkit cp' 2>&1
echo "--- and a missing operand is located in the bash mood ---"
"$BIN" --mood bash -c 'koshkit ls /no/such/path' 2>&1
