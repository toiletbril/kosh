unset KOSH_FLAGS
directory=
trap 'test -n "$directory" && /bin/rm -rf "$directory"' EXIT
# A function runs in the mood it was defined in. A function defined in bash mood
# expands an unset variable to empty even after the session switches to the
# strict default, and a function defined in the default mood stays strict even
# when it is called from bash mood.
echo "== bash-defined function stays lax after switching to kosh:"
"$BIN" -c 'set --mood bash; f() { echo "[${UNSET}]"; }; set --mood kosh; f; echo ok'
echo "== kosh-defined function stays strict when called from bash mood:"
"$BIN" -c 'f() { echo "[${UNSET}]"; }; set --mood bash; f' 2>&1 | grep -o "is not set" | head -1
echo "== sourced function keeps its source mood:"
directory=$(mktemp -d)
printf '%s\n' 'f() { printf "inside=%s unset=[%s]\n" "$(set --mood)" "$NEVER_SET"; }' > "$directory/functions"
"$BIN" -M bash -c ". '$directory/functions'; set --mood kosh; f; printf 'outside=%s\\n' \"\$(set --mood)\""
echo "== idempotent strictness changes survive the defining mood:"
"$BIN" -c '
set --mood bash
f() { set +u; set +o pipefail; set +o failglob; }
set --mood kosh
f
[[ -o nounset ]] || echo nounset=off
[[ -o pipefail ]] || echo pipefail=off
[[ -o failglob ]] || echo failglob=off
'
echo "== propagated mood carries its strictness:"
"$BIN" -c '# shellcheck disable=SC2154
set --mood bash
f() { set --mood sh; }
set --mood kosh
f
printf "mood=%s unset=[%s]\n" "$(set --mood)" "$NEVER_SET"
'
test -n "$directory" && /bin/rm -rf "$directory"
directory=
echo "rc-done"
