# The driver lists command-position candidates for a prefix and respects a
# registered spec, the engine end to end without a tty. PATH is pinned to an
# empty directory so a host binary such as exportfs cannot join the candidates.
DIRECTORY=$(mktemp -d)
trap 'rm -rf "$DIRECTORY"' EXIT
export PATH="$DIRECTORY"
echo "== command prefix:"
"$BIN" --debug-complete-at 'expor' </dev/null
echo "== recent builtin prefix:"
"$BIN" --debug-complete-at 'fc' </dev/null
echo "== spec words filter by prefix:"
"$BIN" -c 'complete -W "fetch fold format" gadget' --debug-complete-at 'gadget f' </dev/null
