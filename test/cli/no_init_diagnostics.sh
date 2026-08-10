unset KOSH_FLAGS
# -WWW reports an unset variable read in the rc. --no-init-diagnostics silences
# the startup stage while keeping -WWW for the session.
home=$(mktemp -d)
trap 'rm -rf "$home"' EXIT
printf 'echo "rc[${UNSET_IN_RC}]"\n' > "$home/.koshrc"
echo "== -WWW warns during init:"
HOME="$home" "$BIN" -WWW -i </dev/null 2>&1 | grep -c "is not set"
echo "== --no-init-diagnostics silences init:"
HOME="$home" "$BIN" -WWW --no-init-diagnostics -i </dev/null 2>&1 | grep -c "is not set"
echo "== -WWW stays active for the session:"
"$BIN" -WWW --no-init-diagnostics -c 'echo "[${UNSET_AT_PROMPT}]"' 2>&1 | grep -c "is not set"
