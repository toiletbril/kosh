unset KOSH_FLAGS
output=$(mktemp)
trap 'rm -f "$output"' EXIT
printf 'echo piped-script\n' | "$BIN" --no-init-files -s > "$output" 2>&1
status=$?
cat "$output"
printf 'rc=%s\n' "$status"
