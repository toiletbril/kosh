# The shitbox builtin completes its utility names in the first operand slot and
# each utility's flags after it, all from the registered FLAG lists. A bare
# utility name completes its own flags when the shitbox option resolves it as a
# command.
DIRECTORY=$(mktemp -d) || exit 1
trap '[ -n "$DIRECTORY" ] && /bin/rm -rf "$DIRECTORY"' EXIT
printf '#!/bin/sh\nexit 0\n' > "$DIRECTORY/ls"
/bin/chmod +x "$DIRECTORY/ls"

echo "== shitbox utilities by prefix:"
"$BIN" --debug-complete-at 'shitbox m' </dev/null
echo "== ls flags through shitbox:"
"$BIN" --debug-complete-at 'shitbox ls -' </dev/null
echo "== du flags through shitbox:"
"$BIN" --debug-complete-at 'shitbox du -' </dev/null
echo "== timeout flags through shitbox:"
"$BIN" --debug-complete-at 'shitbox timeout -' </dev/null
echo "== nproc flags through shitbox:"
"$BIN" --debug-complete-at 'shitbox nproc -' </dev/null
echo "== cat flags through shitbox:"
"$BIN" --debug-complete-at 'shitbox cat --s' </dev/null
echo "== shitbox own flags:"
"$BIN" --debug-complete-at 'shitbox --' </dev/null
echo "== bare utility flags under set -o shitbox:"
"$BIN" -c 'PATH=; set -o shitbox' --debug-complete-at 'ls -' </dev/null
echo "== bare utility names in the default mood:"
"$BIN" -c 'PATH=' --debug-complete-at 'whoa' </dev/null
echo "== bare utility names stay hidden in bash mood:"
"$BIN" -M bash -c 'PATH=' --debug-complete-at 'whoa' </dev/null
echo "== bare utility names appear after set -o shitbox:"
"$BIN" -M bash -c 'PATH=; set -o shitbox' \
  --debug-complete-at 'whoa' </dev/null
echo "== a PATH program keeps its own flags:"
env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$DIRECTORY" \
  "$BIN" -c 'set -o shitbox' --debug-complete-at 'ls -A' </dev/null
