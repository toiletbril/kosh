# A binary reached through a koshkit utility name acts as that utility, the
# busybox multicall. The shell binary is symlinked to ls in a temporary
# directory, and running that symlink lists the directory.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1

"$BIN" -c 'koshkit seq 1 > one.txt'
"$BIN" -c 'koshkit seq 1 > two.txt'
ln -s "$BIN" ls

echo "--- binary named ls acts as ls ---"
./ls
echo "rc=$?"

ln -s "$BIN" tail

echo "--- unknown flag on a symlinked utility reports an error, not a crash ---"
./tail --bogus
echo "rc=$?"

echo "--- a symlinked utility reports the kosh version on --version ---"
./tail --version | grep -c "Koshka Shell"

unset KOSH_FLAGS
# koshkit --assimilate installs a symlink to the binary named for each utility
# into a directory, the busybox-style install. A symlinked invocation routes its
# own flags to the utility rather than the shell CLI. A hermetic temp directory
# keeps it stable, and it is left in place so the test never runs rm.
dir=$(mktemp -d)
"$BIN" -c "koshkit --assimilate '$dir'" </dev/null
echo "== a symlink was installed for head:"
[ -L "$dir/head" ] && echo head-linked || echo head-missing
echo "== a symlinked head routes its own -c flag to the utility:"
printf 'abcdefgh' > "$dir/sample"
"$dir/head" -c 3 "$dir/sample"
echo ""
echo "== a symlinked ls routes -A to ls (count of the dot file):"
: > "$dir/.dotfile"
"$dir/ls" -A -1 "$dir" | grep -c '^\.dotfile$'
