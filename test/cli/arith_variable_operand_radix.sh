unset KOSH_FLAGS
# A variable whose value is a single integer literal parses through one scan,
# so a hex, binary, octal, or negative operand read from a name evaluates the
# same as a bare literal, and an overflowing add wraps modulo 2^64.
"$BIN" -c 'h=0xff; b=0b101; o=010; d=42; n=-0x10; echo $((h)) $((b)) $((o)) $((d)) $((n))'
"$BIN" -c 'h=0xff; b=0b101; echo $((h + b)) $((h * 2))'
"$BIN" -c 'v=0x7fffffffffffffff; echo $((v + 1))'
echo "rc=$?"
