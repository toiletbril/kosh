unset KOSH_FLAGS
# The analysis prepass cannot know the runtime search path, so a command that
# only resolves after a PATH assignment earlier in the script must not be
# reported as not found. An unrelated assignment still lets the check run.
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf '#!/bin/sh\necho ran\n' > "$dir/laterprobe"
chmod +x "$dir/laterprobe"

"$BIN" -c "PATH=\"$dir$TEST_PATH_SEPARATOR\$PATH\"; laterprobe"
"$BIN" -c "export PATH=\"$dir$TEST_PATH_SEPARATOR\$PATH\"; laterprobe"
"$BIN" -c "PATH=\"$dir$TEST_PATH_SEPARATOR\$PATH\" laterprobe"
"$BIN" -c 'FOO=1; definitely_absent_cmd' 2>&1 | sed 's/^[0-9]*:[0-9]*: //' | ./normalize-trace.sh "$BIN"
echo "rc=$?"
