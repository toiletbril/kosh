# The koshkit builtin completes its utility names in the first operand slot and
# each utility's flags after it, all from the registered FLAG lists. A bare
# utility name completes its own flags when the koshkit option resolves it as a
# command.
DIRECTORY=$(mktemp -d) || exit 1
trap '[ -n "$DIRECTORY" ] && /bin/rm -rf "$DIRECTORY"' EXIT
printf '#!/bin/sh\nexit 0\n' > "$DIRECTORY/ls"
/bin/chmod +x "$DIRECTORY/ls"

echo "== koshkit utilities by prefix:"
"$BIN" --debug-complete-at 'koshkit m' </dev/null
echo "== ls flags through koshkit:"
"$BIN" --debug-complete-at 'koshkit ls -' </dev/null
echo "== du flags through koshkit:"
"$BIN" --debug-complete-at 'koshkit du -' </dev/null
echo "== timeout flags through koshkit:"
"$BIN" --debug-complete-at 'koshkit timeout -' </dev/null
echo "== nproc flags through koshkit:"
"$BIN" --debug-complete-at 'koshkit nproc -' </dev/null
echo "== cat flags through koshkit:"
"$BIN" --debug-complete-at 'koshkit cat --s' </dev/null
echo "== date format directives through koshkit:"
"$BIN" --debug-complete-at 'koshkit date +' </dev/null
echo "== koshkit own flags:"
"$BIN" --debug-complete-at 'koshkit --' </dev/null
echo "== bare utility flags under set -o koshkit:"
"$BIN" -c 'PATH=; set -o koshkit' --debug-complete-at 'ls -' </dev/null
echo "== bare utility names in the default mood:"
"$BIN" -c 'PATH=' --debug-complete-at 'whoa' </dev/null
echo "== bare utility names stay hidden in bash mood:"
"$BIN" -M bash -c 'PATH=' --debug-complete-at 'whoa' </dev/null
echo "== bare utility names appear after set -o koshkit:"
"$BIN" -M bash -c 'PATH=; set -o koshkit' \
  --debug-complete-at 'whoa' </dev/null
echo "== a PATH program keeps its own flags:"
env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$DIRECTORY" \
  "$BIN" -c 'set -o koshkit' --debug-complete-at 'ls -A' </dev/null
