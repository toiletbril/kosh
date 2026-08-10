"$BIN" -c '
before_mask=$(umask)
(pushd / >/dev/null; umask 077)
after_mask=$(umask)
[ "$before_mask" = "$after_mask" ] && echo mask-restored
[ "$(dirs -p | koshkit wc -l)" -eq 1 ] && echo directory-stack-restored
'

directory=$(mktemp -d)
trap '[ -n "$directory" ] && /bin/rm -rf "$directory"' EXIT
/bin/mkdir -p "$directory/original" "$directory/sibling"
: > "$directory/original/marker"
SUBSHELL_DIRECTORY=$directory "$BIN" -c '
cd "$SUBSHELL_DIRECTORY/original" || exit 1
(chmod 000 .; cd "$SUBSHELL_DIRECTORY/sibling")
chmod 700 "$SUBSHELL_DIRECTORY/original"
[[ -f marker ]] && echo permission-directory-restored
'
chmod 700 "$directory/original"
