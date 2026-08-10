unset KOSH_FLAGS
# pushd saves the current directory and changes to its argument, dirs prints the
# stack with the current directory first, popd returns to the top, and +N or -N
# rotates or drops an entry. The home directory abbreviates to ~.
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/a" "$dir/b" "$dir/c" "$dir/home"
export HOME=$dir/home
real_dir=$(cd "$dir" && pwd -P)

scrub() {
    sed 's|\\|/|g' | sed "s|$real_dir|D|g; s|$dir|D|g" | awk '
    /\^~~~$/ {
        print "       |                            ^~~~"
        next
    }
    { print }
    '
}

echo "== dirs on an empty stack shows only the current directory:"
"$BIN" -c "cd '$dir' || exit; dirs" 2>&1 | scrub
echo "== pushd builds the stack, current first:"
"$BIN" -c "cd '$dir' || exit; pushd '$dir/a'; pushd '$dir/b'; dirs" 2>&1 | scrub
echo "== dirs -v numbers each entry from the top:"
"$BIN" -c "cd '$dir' || exit; pushd '$dir/a' >/dev/null; pushd '$dir/b' >/dev/null; dirs -v" 2>&1 | scrub
echo "== popd returns through the stack:"
"$BIN" -c "cd '$dir' || exit; pushd '$dir/a' >/dev/null; pushd '$dir/b' >/dev/null; popd; popd; echo PWD=\$PWD" 2>&1 | scrub
echo "== pushd with no argument swaps the top two:"
"$BIN" -c "cd '$dir' || exit; pushd '$dir/a' >/dev/null; pushd; echo PWD=\$PWD" 2>&1 | scrub
echo "== pushd +1 rotates the Nth entry to the top:"
"$BIN" -c "cd '$dir' || exit; pushd '$dir/a' >/dev/null; pushd '$dir/b' >/dev/null; pushd +1; echo PWD=\$PWD" 2>&1 | scrub
echo "== popd +1 drops a saved entry without a chdir:"
"$BIN" -c "cd '$dir' || exit; pushd '$dir/a' >/dev/null; pushd '$dir/b' >/dev/null; popd +1; dirs; echo PWD=\$PWD" 2>&1 | scrub
echo "== dirs -c clears the stack:"
"$BIN" -c "cd '$dir' || exit; pushd '$dir/a' >/dev/null; dirs -c; dirs" 2>&1 | scrub
echo "== a forged PWD does not change the saved directory:"
"$BIN" -c "cd '$dir/a' || exit; PWD='$dir/c'; pushd '$dir/b' >/dev/null; dirs" 2>&1 | scrub
echo "== the home directory abbreviates to ~:"
HOME="$dir" "$BIN" -c "cd '$dir' || exit; pushd '$dir/a'" 2>&1 | scrub
echo "== popd on an empty stack reports an error:"
"$BIN" -c "cd '$dir' || exit; popd" 2>&1 | scrub | sed 's/^kosh: [0-9]*:[0-9]*: //'
