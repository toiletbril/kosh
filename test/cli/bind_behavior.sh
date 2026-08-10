unset KOSH_FLAGS
# bind is a no-op stub. The flags and the operands are accepted without
# effect, and the builtin returns zero. The -m and -l forms and a key binding
# operand all pass through.
echo "== bind -m returns 0:"
"$BIN" -c 'bind -m'; echo "rc=$?"
echo "== bind -l returns 0:"
"$BIN" -c 'bind -l'; echo "rc=$?"
echo "== bind with a keyseq returns 0:"
"$BIN" -c 'bind "\C-x: abort"'; echo "rc=$?"
