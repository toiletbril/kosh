# The ls listing modes run inside a fresh temporary directory so the output names
# no absolute path and stays the same on every machine. The binary path is
# resolved to an absolute one first, since the working directory changes.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1

mkdir -p sub/deep empty sized
printf 'aaa\n' > plain.txt
printf '#!/bin/sh\n' > run.sh
chmod +x run.sh
ln -s plain.txt good-link
ln -s nowhere bad-link
: > sub/inner.txt
: > sub/deep/leaf.txt
# The size sort reads regular files alone, because the size a directory reports
# differs between filesystems.
printf 'a\n' > sized/small
printf 'bbbbb\n' > sized/medium
printf 'cccccccccc\n' > sized/large

echo "--- plain ---"
"$BIN" -c 'koshkit ls'
echo "--- classify ---"
"$BIN" -c 'koshkit ls -F'
echo "--- reverse name ---"
"$BIN" -c 'koshkit ls -r'
echo "--- sort by size ---"
"$BIN" -c 'koshkit ls -S sized'
echo "--- sort by size reversed ---"
"$BIN" -c 'koshkit ls -Sr sized'
echo "--- color never is bare ---"
"$BIN" -c 'koshkit --color never ls -F'
echo "--- color always ---"
"$BIN" -c 'koshkit --color always ls -F' | cat -v
echo "--- redirected output carries no escape ---"
"$BIN" -c 'koshkit ls -F' | cat -v
echo "--- human total ---"
human_total=$("$BIN" -c 'koshkit ls -lah sized' | head -n 1)
case $human_total in
  total\ *[KMGTP]) echo "human-total=matched" ;;
  *) echo "human-total=wrong" ;;
esac
echo "--- tree ---"
"$BIN" -c 'koshkit ls --tree sub'
echo "--- tree bounded to one level ---"
"$BIN" -c 'koshkit ls --tree -L 1 sub'
echo "--- recursive ---"
"$BIN" -c 'koshkit ls -R sub'
echo "--- recursive bounded to one level ---"
"$BIN" -c 'koshkit ls -R -L 1 sub'
echo "--- recursive reaches an empty directory ---"
"$BIN" -c 'koshkit ls -R empty'
echo "--- invalid level ---"
"$BIN" -c 'koshkit ls -L 0 sub' 2>/dev/null
echo "rc=$?"
echo "--- invalid color ---"
"$BIN" -c 'koshkit --color pink ls sub' 2>/dev/null
echo "rc=$?"

cd / || exit 1
rm -rf "$d"
