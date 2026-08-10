unset KOSH_FLAGS
# A -c in KOSH_FLAGS is dropped during the splice so the environment cannot
# inject a command into every invocation, while a benign flag there survives
# and a real command-line -c still runs.
echo "== -c in KOSH_FLAGS does not run its command:"
KOSH_FLAGS='-c injected_command' "$BIN" -c 'echo clean' 2>&1
echo "== benign flag in KOSH_FLAGS survives:"
KOSH_FLAGS='--mood sh' "$BIN" -c 'echo flagged' 2>&1
echo "rc=$?"
