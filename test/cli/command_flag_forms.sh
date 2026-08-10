unset KOSH_FLAGS
"$BIN" -c 'echo one' -c 'echo two'
"$BIN" --command='echo equals-form'
"$BIN" -E -c 'echo trailer'
echo "rc=$?"
