unset KOSH_FLAGS
# A name that is not a builtin is a located error pointing at the operand.
echo "== help nope is a located error:"
"$BIN" -c 'help nope' 2>&1; echo "rc=$?"
