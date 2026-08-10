unset SHIT_FLAGS
# A braced variable path keeps its source spelling, and a bare reference still
# completes variable names.
DIRECTORY=$(mktemp -d)
trap 'rm -rf "$DIRECTORY"' EXIT
mkdir -p "$DIRECTORY/Downloads" "$DIRECTORY/Documents"

echo "== \${HOME}/ braced:"
HOME="$DIRECTORY" "$BIN" --debug-complete-at 'cat ${HOME}/Doc' </dev/null
echo "== a bare variable reference still completes names:"
VARIABLE_COMPLETION_UNIQUE="$DIRECTORY" \
  "$BIN" --debug-complete-at 'echo $VARIABLE_COMPLETION_UNI' </dev/null
echo "== shit git dynamic variables complete:"
"$BIN" --debug-complete-at 'echo $SHIT_GIT_' </dev/null
echo "== bash dynamic variables complete in bash mood:"
"$BIN" -M bash --debug-complete-at 'echo $BASHPI' </dev/null
echo "== POSIX mood completes its dynamic variables:"
"$BIN" -M sh --debug-complete-at 'echo $LINE' </dev/null
