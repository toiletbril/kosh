# The rm koshkit utility. POLICY: every rm invocation in an rm test uses
# --dry-run, so the test verifies what rm would remove without ever deleting a
# real file. The dangling-symlink case proves rm sees a broken link, since it
# would remove it rather than report it absent. The guards refuse '.' and the
# root before any removal, and the dry run prints in child-before-parent order.
unset KOSH_FLAGS
initial_directory=$PWD
d=$(mktemp -d)
cd "$d" || exit 1
echo "== rm sees a dangling symlink, so it would remove it, not report it absent:"
: > target
ln -s target link
mv target target-away
"$BIN" -c 'koshkit rm --dry-run link' 2>&1
echo "== --dry-run prints without deleting a file:"
: > keep.txt
"$BIN" -c 'koshkit rm --dry-run keep.txt' 2>&1
[ -e keep.txt ] && echo "keep.txt still present"
echo "== rm -i honors both answers under --dry-run:"
declined=$(printf 'n\n' | "$BIN" -c 'koshkit rm -i --dry-run keep.txt' 2>/dev/null)
printf 'declined=%s\n' "${declined:-empty}"
accepted=$(printf 'y\n' | "$BIN" -c 'koshkit rm -i --dry-run keep.txt' 2>/dev/null)
printf 'accepted=%s\n' "$accepted"
echo "== --dry-run -r lists a tree in removal order without deleting it:"
mkdir -p tree/sub
: > tree/sub/b
"$BIN" -c 'koshkit rm -r --dry-run tree' 2>&1 | sed 's|\\|/|g'
[ -e tree ] && echo "tree still present"
echo "== rm refuses '.' and the root directory, before any removal:"
"$BIN" -c 'koshkit rm --dry-run .' 2>&1
"$BIN" -c 'koshkit rm --dry-run /' 2>&1
cd "$initial_directory" || exit 1
[ -n "$d" ] && rm -rf "$d"

unset KOSH_FLAGS
# rm refuses a . or .. operand even under -f, so a recursive remove cannot delete
# the working or the parent directory entry. POLICY: every rm here uses
# --dry-run, so the test never deletes a real file. The refusal fires before any
# removal, so the count is unchanged under a dry run.
dir=$(mktemp -d)
: > "$dir/keep"
echo "== rm -rf . is refused (count of the refusal):"
"$BIN" -c "cd '$dir' || exit; koshkit rm -rf --dry-run ." </dev/null 2>&1 | grep -c "refusing to remove"
echo "== rm -rf .. is refused (count):"
"$BIN" -c "cd '$dir' || exit; koshkit rm -rf --dry-run .." </dev/null 2>&1 | grep -c "refusing to remove"
echo "== the directory contents survive:"
"$BIN" -c "if [ -e '$dir/keep' ]; then echo keep-survives; else echo keep-gone; fi" </dev/null
[ -n "$dir" ] && rm -rf "$dir"

unset KOSH_FLAGS
# rm refuses a bare root operand even under -f, so a recursive remove cannot walk
# the whole filesystem from /, matching GNU rm under its default preserve-root.
# POLICY: every rm here uses --dry-run, so the test never deletes a real file. A
# normal operand is reported for removal under the dry run while the file
# survives.
dir=$(mktemp -d)
: > "$dir/keep"
echo "== rm -rf / is refused (count of the refusal):"
"$BIN" -c "koshkit rm -rf --dry-run /" </dev/null 2>&1 | grep -c "refusing to remove the root"
echo "== rm -rf // is refused (count):"
"$BIN" -c "koshkit rm -rf --dry-run //" </dev/null 2>&1 | grep -c "refusing to remove the root"
echo "== a normal file is reported for removal under --dry-run, and survives:"
"$BIN" -c "cd '$dir' || exit; koshkit rm --dry-run keep" </dev/null 2>&1
"$BIN" -c "if [ -e '$dir/keep' ]; then echo keep-survives; else echo keep-removed; fi" </dev/null
[ -n "$dir" ] && rm -rf "$dir"
