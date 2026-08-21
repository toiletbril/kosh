set -e

null_device=${TEST_NULL_DEVICE:-/dev/null}
result=$("$BIN" --debug-highlight-at "echo x >$null_device")
tab=$(printf '\t')
printf '%s\n' "$result" | grep -Fx "${null_device}${tab}existing-path" >/dev/null
printf '/dev/null\texisting-path\n'
