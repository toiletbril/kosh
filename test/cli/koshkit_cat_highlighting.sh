set -e

unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
starting_directory=$PWD
trap 'cd "$starting_directory" && test -n "$d" && /bin/rm -rf "$d"' EXIT
cd "$d" || exit 1

printf 'echo "extension"\n' > extension.sh
printf '#!/usr/bin/env bash\necho "shebang"\n' > detected
printf 'plain\n' > plain.txt
printf 'first\n' > first.txt
printf 'last\n' > last.txt
printf 'joined' > joined.txt
printf 'line\n' > line.txt
printf 'raw\000bytes\n' > binary.sh

echo '--- redirected shell sources stay plain ---'
"$BIN" -c 'koshkit cat --syntax-highlighting extension.sh detected plain.txt'
echo '--- numbering continues through stdin and files ---'
printf 'middle\n' | "$BIN" -c 'koshkit cat --syntax-highlighting -n first.txt - last.txt'
"$BIN" -c 'koshkit cat --syntax-highlighting -n joined.txt line.txt'
echo '--- binary shell files remain byte-exact ---'
"$BIN" -c 'koshkit cat --syntax-highlighting binary.sh' > binary.out
cmp binary.sh binary.out && echo exact
