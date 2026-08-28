unset KOSH_FLAGS
# A radix literal stored in a variable has the same value as a bare literal.
# Default mood addition remains exact beyond the signed 64-bit range.
"$BIN" -c 'h=0xff; b=0b101; o=010; d=42; n=-0x10; echo $((h)) $((b)) $((o)) $((d)) $((n))'
"$BIN" -c 'h=0xff; b=0b101; echo $((h + b)) $((h * 2))'
"$BIN" -c 'v=0x7fffffffffffffff; echo $((v + 1))'
echo "rc=$?"
