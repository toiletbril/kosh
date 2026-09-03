unset KOSH_FLAGS
output=$(mktemp)
trap 'rm -f "$output"' EXIT
printf 'echo piped-script\n' | "$BIN" --no-init-files -s > "$output" 2>&1
status=$?
cat "$output"
printf 'rc=%s\n' "$status"
printf '' | "$BIN" --no-init-files -s
printf 'empty-stdin-rc=%s\n' "$?"
printf '%s' "echo \$'abc\\" | "$BIN" --no-init-files -s > "$output" 2>&1
status=$?
grep -F "Unterminated \$'...' string." "$output" >/dev/null || exit 1
printf 'unterminated-ansi-backslash-rc=%s\n' "$status"
