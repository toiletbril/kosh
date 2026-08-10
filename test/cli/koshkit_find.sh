# The find utility walks a fixed temporary tree, so the relative paths it prints
# stay the same on every machine. The children of a directory are listed in
# sorted order, so the whole walk is deterministic.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1

"$BIN" -c 'koshkit mkdir -p a/b/c'
"$BIN" -c 'koshkit touch a/one.txt'
"$BIN" -c 'koshkit touch a/b/two.log'
"$BIN" -c 'koshkit touch a/b/c/three.txt'

echo "--- find all ---"
"$BIN" -c 'koshkit find .'
echo "--- find -name *.txt ---"
"$BIN" -c 'koshkit find . -name "*.txt"'
echo "--- find -type d ---"
"$BIN" -c 'koshkit find . -type d'
echo "--- find -maxdepth 1 ---"
"$BIN" -c 'koshkit find . -maxdepth 1'
echo "--- find -mindepth 3 -type f ---"
"$BIN" -c 'koshkit find . -mindepth 3 -type f'
echo "--- find a named root ---"
"$BIN" -c 'koshkit find a/b'
echo "--- find unknown predicate ---"
"$BIN" -c 'koshkit find . -bogus' 2>&1
echo "--- find missing -name argument ---"
"$BIN" -c 'koshkit find . -name' 2>&1

unset KOSH_FLAGS
# find rejects a negative -maxdepth or -mindepth rather than reading it as the
# unlimited sentinel, so a typo'd negative depth errors instead of walking the
# whole tree unbounded.
echo "== -maxdepth -1 is rejected:"
"$BIN" -c "koshkit find . -maxdepth -1" </dev/null 2>&1 | grep -c "non-negative number"
echo "== -mindepth -3 is rejected:"
"$BIN" -c "koshkit find . -mindepth -3" </dev/null 2>&1 | grep -c "non-negative number"
echo "== a valid -maxdepth 0 still works (lists only the root):"
"$BIN" -c "koshkit find . -maxdepth 0" </dev/null
