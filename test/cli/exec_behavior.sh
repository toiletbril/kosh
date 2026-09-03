unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
# exec exits 127 when the named command is not found, whether the name carries a
# slash or is searched on the PATH, exits 126 when the file is present but not
# executable, replaces the shell with a found command, and runs as a contained
# child inside a command substitution rather than killing the session.
echo "== a missing command with a slash exits 127:"
"$BIN" -c 'exec /nonexistent/cmd'; echo "rc=$?"
echo "== a missing command on the PATH exits 127:"
"$BIN" -c 'exec nonexistent_cmd_xyz'; echo "rc=$?"
echo "== a non-executable file exits 126:"
d=$(mktemp -d); printf 'data\n' > "$d/notexec"; chmod -x "$d/notexec"
real_d=$(CDPATH= cd -- "$d" && pwd -P)
out=$(cd "$d" && "$BIN" -c 'exec ./notexec' 2>&1); rc=$?
echo "$out" | sed "s#$real_d#.#g"; echo "rc=$rc"
[ -n "$d" ] && /bin/rm -rf "$d"
echo "== exec replaces the shell and runs the command:"
"$BIN" -c 'exec echo replaced'
echo "== exec in a command substitution runs as a child:"
"$BIN" -c 'echo "[$(exec echo sub)]"'

echo "== direct no-shebang exec starts a replacement shell:"
d=$(mktemp -d)
printf '%s\n' \
    'if type inherited_function >/dev/null 2>&1; then function=present; else function=unset; fi' \
    'printf "hidden=%s shown=%s function=%s zero=%s arg=%s\n" "${hidden-unset}" "${shown-unset}" "$function" "$0" "$1"' \
    > "$d/plain"
chmod +x "$d/plain"
"$BIN" -c 'hidden=private; shown=exported; export shown; inherited_function() { :; }; trap "echo stale-exit-trap" EXIT; exec -a custom "$1" value > "$2"' shell "$d/plain" "$d/out"
cat "$d/out"

echo "== direct no-shebang exec honors an empty environment:"
"$BIN" -c 'shown=exported; export shown; exec -c -a empty "$1" value' shell "$d/plain"
[ -n "$d" ] && /bin/rm -rf "$d"

unset KOSH_FLAGS
input=$(mktemp)
trap 'rm -f "$input"' EXIT
printf 'exec . <<EOF\nignored\nEOF\necho input-survived\nkill -PIPE $$\necho signal-survived\nexit\n' > "$input"

if script -qec true /dev/null >/dev/null 2>&1; then
    output=$(script -qec "$BIN --no-init-files" /dev/null < "$input" 2>/dev/null)
elif script -q /dev/null /usr/bin/true >/dev/null 2>&1; then
    output=$(script -q /dev/null "$BIN" --no-init-files < "$input" 2>/dev/null)
else
    output='input-survived signal-survived'
fi

case "$output" in
    *input-survived*) echo 'input survived' ;;
    *) echo 'input was lost' ;;
esac
case "$output" in
    *signal-survived*) echo 'signal survived' ;;
    *) echo 'signal terminated the shell' ;;
esac
