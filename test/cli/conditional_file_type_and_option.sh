unset SHIT_FLAGS
directory=$(mktemp -d)
trap '[ -n "$directory" ] && /bin/rm -rf "$directory"' EXIT
plain=$directory/plain
: > "$plain"

echo "== file-type primaries follow the platform:"
if [ "${OS-}" = Windows_NT ]; then
    "$BIN" -c "[[ -c /dev/null && ! -c '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[[ ! -p '$plain' && ! -b '$plain' && ! -S '$plain' ]]" \
        </dev/null || exit 1
    "$BIN" -c "[[ ! -O '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[ ! -p '$plain' ] && [ ! -u '$plain' ] && [ ! -O '$plain' ]" \
        </dev/null || exit 1
elif "$BIN" -c '[[ -c /dev/null ]]' </dev/null; then
    fifo=$directory/fifo
    mkfifo "$fifo"
    "$BIN" -c "[[ -p '$fifo' && ! -p '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[[ -c /dev/null && ! -c '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[[ ! -b '$plain' && ! -S '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[[ -O '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[ -p '$fifo' ] && [ ! -u '$plain' ] && [ -O '$plain' ]" \
        </dev/null || exit 1
else
    "$BIN" -c "[[ ! -p '$plain' && ! -c '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[[ ! -b '$plain' && ! -S '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[[ ! -O '$plain' ]]" </dev/null || exit 1
    "$BIN" -c "[ ! -p '$plain' ] && [ ! -u '$plain' ] && [ ! -O '$plain' ]" \
        </dev/null || exit 1
fi
echo file-types-ok

echo "== -o reads the real state of a shell option:"
"$BIN" -c "set -o pipefail; if [[ -o pipefail ]]; then echo pipefail-on; else echo pipefail-off; fi" </dev/null
"$BIN" -c "set +o pipefail; if [[ -o pipefail ]]; then echo pipefail-on; else echo pipefail-off; fi" </dev/null
"$BIN" -c "if [[ -o emacs ]]; then echo emacs-on; else echo emacs-off; fi" </dev/null
"$BIN" -c "if [[ -o no_such_option_xyz ]]; then echo unknown-on; else echo unknown-off; fi" </dev/null
