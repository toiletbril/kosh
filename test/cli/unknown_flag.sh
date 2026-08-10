unset KOSH_FLAGS
# The caret line renders the argv[0] the harness invoked, which varies by host,
# so the test asserts the message text and the exit code, not the caret.
out=$("$BIN" --zz-bogus-flag 2>&1)
rc=$?
printf '%s\n' "$out" | grep -o "error: Unknown flag .*"
echo "rc=$rc"
