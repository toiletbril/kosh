unset KOSH_FLAGS
# command -p resolves its operand against a default PATH, then the resolver
# must revert to the shell's own PATH. A bogus or a valid operand once left the
# resolver with no PATH, so every later command stopped being found.
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf '#!/bin/sh\necho probe-ran\n' > "$dir/probecmd"
chmod +x "$dir/probecmd"
export COMMAND_P_OUTPUT=$dir/command-p-output

env -u PATH \
    "$TEST_PATH_ENVIRONMENT_NAME=$dir${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
    "$BIN" -c 'command -p nonexistent_xyz; probecmd' \
    2>"$COMMAND_P_OUTPUT"
if [ "${OS-}" = Windows_NT ]; then
    env -u PATH \
        "$TEST_PATH_ENVIRONMENT_NAME=$dir${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
        "$BIN" -c 'command -p "$BIN" -c true > "$COMMAND_P_OUTPUT"; probecmd'
else
    PATH="$dir${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
        "$BIN" -c 'command -p ls > "$COMMAND_P_OUTPUT"; probecmd'
fi
echo "rc=$?"
