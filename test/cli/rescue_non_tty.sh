unset KOSH_FLAGS
# A bad flag left in KOSH_FLAGS exits with the usage status when standard input
# is not a terminal, so a script or a pipe sees the error and the non-zero
# status. The rescue prompt is reserved for an interactive terminal, so a
# non-tty run never prints the rescue banner.
echo "== non-tty bad flag exits 2:"
# The error reconstructs argv, which carries the binary path, so the test keeps
# only the path-independent error text and the exit status. The golden then does
# not shift when the suite runs a mode-suffixed binary such as kosh-dbg.
rescue_output=$(KOSH_FLAGS='--nonexistent-flag' "$BIN" -c 'echo unreached' </dev/null 2>&1)
rescue_status=$?
printf '%s\n' "$rescue_output" | grep -o "error: Unknown flag '--nonexistent-flag'."
echo "exit=$rescue_status"
echo "== non-tty run does not enter rescue:"
KOSH_FLAGS='--nonexistent-flag' "$BIN" -c 'echo unreached' </dev/null 2>&1 |
  grep -c "Entering rescue"
