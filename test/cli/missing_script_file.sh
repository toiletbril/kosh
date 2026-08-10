unset KOSH_FLAGS
# The caret line renders the argv[0] the harness invoked, which varies by host
# and platform, so the test asserts the message text and the exit code rather
# than the BIN-dependent caret.
out=$("$BIN" zz_no_such_script_file 2>&1)
rc=$?
case $out in
*"error: Could not open"*) echo "missing script error" ;;
*) printf '%s\n' "$out" ;;
esac
echo "rc=$rc"
