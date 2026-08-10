unset KOSH_FLAGS
# logout in a non-login shell prints a not-login-shell message and returns
# one, since exit is the non-login counterpart. A non-numeric operand is a
# located error pointing at the operand.
echo "== logout in a non-login shell is refused:"
"$BIN" -c 'logout' 2>&1; echo "rc=$?"
echo "== logout with a non-numeric operand is a located error:"
"$BIN" -c 'logout abc' 2>&1; echo "rc=$?"
