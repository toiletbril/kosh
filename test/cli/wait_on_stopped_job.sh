unset KOSH_FLAGS
d=$(mktemp -d)
escaped_cleanup_is_armed=no
cleanup()
{
    if [ "$escaped_cleanup_is_armed" = yes ] && [ -s "$d/escaped-pid" ]; then
        read -r escaped_pid < "$d/escaped-pid"
        kill "$escaped_pid" 2>/dev/null || true
        kill -CONT "$escaped_pid" 2>/dev/null || true
    fi
    [ -n "$d" ] && /bin/rm -rf "$d"
}
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
trap 'exit 129' HUP

"$BIN" --mood bash -c 'sleep 5 & p=$!; kill -STOP "$p"; jobs >/dev/null; wait %1; echo WAIT_RETURNED; kill "$p" 2>/dev/null; kill -CONT "$p" 2>/dev/null' 2>&1 | grep -c WAIT_RETURNED
"$BIN" --mood bash -c 'yes | sh -c '\''kill -STOP "$$"; sleep 30'\'' & wait %1; echo WAIT_RETURNED; kill %1 2>/dev/null; kill -CONT %1 2>/dev/null' 2>&1 | grep -c WAIT_RETURNED
if command -v setsid >/dev/null 2>&1; then
    escaped_cleanup_is_armed=yes
    ESCAPED_PID_FILE="$d/escaped-pid" "$BIN" --mood bash -c 'yes | setsid sh -c '\''echo "$$" > "$ESCAPED_PID_FILE"; kill -STOP "$$"; sleep 30'\'' & wait %1; echo WAIT_RETURNED; kill %1 2>/dev/null; kill -CONT %1 2>/dev/null' 2>&1 | grep -c WAIT_RETURNED
    escaped_cleanup_is_armed=no
else
    echo 1
fi
"$BIN" --mood bash -c '(sleep 0.01 & wait); echo SUBSHELL_RETURNED' 2>&1 | grep -c SUBSHELL_RETURNED
