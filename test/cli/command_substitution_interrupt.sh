#!/bin/bash

d=$(mktemp -d)
shell_pid=
watchdog_pid=
cleanup()
{
    if [ -n "$shell_pid" ]; then
        kill -KILL "$shell_pid" 2>/dev/null
        wait "$shell_pid" 2>/dev/null
    fi
    if [ -n "$watchdog_pid" ]; then
        kill "$watchdog_pid" 2>/dev/null
        wait "$watchdog_pid" 2>/dev/null
    fi
    if [ -n "$d" ]; then
        /bin/rm -rf "$d"
    fi
}
trap cleanup EXIT

"$BIN" --mood bash -c \
    'value=$(echo ready > "$1"; while :; do :; done)' \
    shell "$d/ready" >"$d/output" 2>&1 &
shell_pid=$!

waited=0
while [ ! -s "$d/ready" ] && [ "$waited" -lt 1000 ]; do
    /bin/sleep 0.01
    waited=$((waited + 1))
done
test -s "$d/ready" || exit 1

kill -INT "$shell_pid"
"$BIN" -p --mood sh -c \
    'koshkit sleep 10; kill -KILL "$1" 2>/dev/null' \
    shell "$shell_pid" &
watchdog_pid=$!
wait "$shell_pid"
status=$?
shell_pid=
kill "$watchdog_pid" 2>/dev/null
wait "$watchdog_pid" 2>/dev/null
watchdog_pid=

echo "status=$status"
grep -A2 'error: Interrupted' "$d/output"
if grep -q 'Could not read command substitution output' "$d/output"; then
    echo pipe-read-error
else
    echo clean-interrupt
fi
