# The -n count returns success when the requested bytes are read and failure
# when the input ends first, and either way the field holds what was read.
unset KOSH_FLAGS
printf 'hi' | "$BIN" -c 'read -r -n 2 x; echo "rc=$? x=[$x]"'
printf 'h' | "$BIN" -c 'read -r -n 2 x; echo "rc=$? x=[$x]"'
printf '' | "$BIN" -c 'read -r -n 2 x; echo "rc=$? x=[$x]"'

unset KOSH_FLAGS
# A prefix IFS=... on read must split on that IFS even when a prior assignment
# left a different IFS in the store. sdkman reads its comma-separated candidate
# list this way after setting IFS to a newline, so a stale IFS broke its PATH
# setup.
echo "== a prefix IFS splits read after the global IFS was changed:"
"$BIN" --mood bash -s <<'EOF'
IFS=$'\n'
IFS=',' read -a parts <<< "a,b,c,d"
echo "count=${#parts[@]} first=${parts[0]} last=${parts[3]}"
EOF
echo "== a prefix IFS splits read with the default global IFS:"
"$BIN" --mood bash -s <<'EOF'
IFS=',' read -a parts <<< "x,y,z"
echo "${parts[0]}|${parts[1]}|${parts[2]}"
EOF
echo "== read without a prefix follows the changed global IFS:"
"$BIN" --mood bash -s <<'EOF'
IFS=,
read -a parts <<< "one,two,three"
echo "count=${#parts[@]}"
EOF

echo "== read -q returns zero for y:"
printf 'y\n' | "$BIN" -c 'read -r -q; echo "rc=$?"'
echo "== read -q returns zero for Y:"
printf 'Y\n' | "$BIN" -c 'read -r -q; echo "rc=$?"'
echo "== read -q returns one for n:"
printf 'n\n' | "$BIN" -c 'read -r -q; echo "rc=$?"'
echo "== read -q returns one for a non-y byte:"
printf 'x\n' | "$BIN" -c 'read -r -q; echo "rc=$?"'
echo "== read -q at EOF returns one:"
"$BIN" -c 'read -r -q </dev/null; echo "rc=$?"'

#!/bin/sh

unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")

d=$(mktemp -d) || exit 1
trap '[ -n "$d" ] && /bin/rm -rf "$d"' EXIT

wait_for_marker()
{
    marker_attempt_count=0
    while [ ! -e "$1" ]; do
        [ "$marker_attempt_count" -lt 500 ] || return 1
        sleep 0.01
        marker_attempt_count=$((marker_attempt_count + 1))
    done
}

(
    : > "$d/query-ready"
    wait_for_marker "$d/query-release" || exit 1
    printf a
    : > "$d/count-ready"
    wait_for_marker "$d/count-release" || exit 1
    printf 'a\\\n'
    : > "$d/continuation-ready"
    wait_for_marker "$d/continuation-release" || exit 1
) | READ_MARKER_DIRECTORY=$d "$BIN" -c '
while [ ! -e "$READ_MARKER_DIRECTORY/query-ready" ]; do :; done
read -r -q -t 0.02
printf "query=%s\n" "$?"
: > "$READ_MARKER_DIRECTORY/query-release"

while [ ! -e "$READ_MARKER_DIRECTORY/count-ready" ]; do :; done
read -r -n 3 -t 0.02 value
printf "count=%s value=[%s]\n" "$?" "$value"
: > "$READ_MARKER_DIRECTORY/count-release"

while [ ! -e "$READ_MARKER_DIRECTORY/continuation-ready" ]; do :; done
read -t 0.02 value
printf "continued=%s value=[%s]\n" "$?" "$value"
: > "$READ_MARKER_DIRECTORY/continuation-release"
' 2>/dev/null

if [ "${OS-}" = Windows_NT ]; then
    echo "invalid=1"
else
    "$BIN" -c "read -r -t 0 -u 99 value </dev/null; printf 'invalid=%s\n' \"\$?\""
fi

printf '' | "$BIN" -c "read -r -t 0.1 value; printf 'eof=%s value=[%s]\n' \"\$?\" \"\$value\""
