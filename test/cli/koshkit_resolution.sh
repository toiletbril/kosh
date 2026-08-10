# The koshkit builtin prefix always works. In the default mood a bare coreutil
# name falls back to the koshkit utility when PATH has no binary of that name,
# while the sh mood reports a command not found. The --enable-koshkit flag and
# set -o koshkit enable the same fallback in every mood.
# An empty PATH isolates the resolution from the system coreutils.
unset KOSH_FLAGS

dir=$(mktemp -d) || exit 1
trap '[ -n "$dir" ] && /bin/rm -rf "$dir"' EXIT
printf '#!/bin/sh\nprintf "PATH seq\\n"\n' > "$dir/seq"
/bin/chmod +x "$dir/seq"

echo "=== koshkit prefix always works ==="
"$BIN" -c 'koshkit seq 3'

echo "=== default mood, empty PATH, falls back to koshkit ==="
"$BIN" -c 'PATH=; seq 3'
echo "rc=$?"

echo "=== sh mood, empty PATH, not found ==="
{
    "$BIN" --mood sh -c 'PATH=; seq 3' 2>&1
    printf 'rc=%s\n' "$?"
} | ./normalize-trace.sh "$BIN"

echo "=== set -o koshkit turns bare names on ==="
"$BIN" -c 'PATH=; set -o koshkit; seq 3'

echo "=== --enable-koshkit turns bare names on ==="
"$BIN" --enable-koshkit -c 'PATH=; seq 3'

echo "=== --enable-koshkit prefers a PATH binary ==="
env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$dir" \
    "$BIN" --enable-koshkit -c 'seq 3'

echo "=== set -o koshkit prefers a PATH binary ==="
env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$dir" \
    "$BIN" -c 'set -o koshkit; seq 3'

echo "=== --enable-koshkit works in the sh mood ==="
"$BIN" --mood sh --enable-koshkit -c 'PATH=; seq 3'

echo "=== command reports the enabled fallback ==="
"$BIN" --enable-koshkit -c 'PATH=; command -v seq; command -V seq'

echo "=== which reports the enabled fallback ==="
"$BIN" --enable-koshkit -c 'PATH=; which seq' 2>&1 |
  ./normalize-trace.sh "$BIN"

echo "=== a builtin name is not a koshkit utility ==="
"$BIN" -c 'koshkit echo routed via koshkit' 2>&1
echo "rc=$?"

echo "=== the koshkit name does not recurse ==="
"$BIN" -c 'koshkit koshkit' 2>&1
echo "rc=$?"

echo "=== unknown utility errors ==="
"$BIN" -c 'koshkit nope' 2>&1
echo "rc=$?"
