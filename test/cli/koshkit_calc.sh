# calc evaluates arithmetic through the shell evaluator and computes in 128 bits,
# so 2 ** 100 prints in full. A malformed expression renders a located caret.
# calc is a koshkit utility, so the bundled evaluator is reached through
# `koshkit calc`, which a calc binary on PATH does not shadow.
unset KOSH_FLAGS

echo "=== precedence ==="
"$BIN" -c 'koshkit calc "2 + 3 * 4"'

echo "=== parentheses and power ==="
"$BIN" -c 'koshkit calc "(1 + 2) ** 3"'

echo "=== 128-bit in the default mood ==="
"$BIN" -c 'koshkit calc "2 ** 100"'

echo "=== beyond 128-bit in the default mood ==="
"$BIN" -c 'koshkit calc "2 ** 256"'

echo "=== signed 128-bit minimum ==="
"$BIN" -c 'koshkit calc -- "-170141183460469231731687303715884105728"'
"$BIN" -c 'koshkit calc "(2 ** 127) / -1"'
"$BIN" -c 'koshkit calc "(2 ** 127) % -1"'

echo "=== variable read ==="
"$BIN" -c 'x=6; koshkit calc "x * 7"'

echo "=== common functions ==="
"$BIN" -c '
koshkit calc "abs(-12.50)"
koshkit calc "floor(-1.2)"
koshkit calc "ceil(1.2)"
koshkit calc "frac(-7.25)"
koshkit calc "int(-7.25)"
koshkit calc "cmp(999999999999999999999, 8)"
koshkit calc "gcd(84, 30, 18)"
koshkit calc "lcm(12, 18)"
koshkit calc "fact(30)"
koshkit calc "fib(100)"
koshkit calc "min(9.2, -4, 7)"
koshkit calc "max(9.2, -4, 7)"
koshkit calc "isint(1.00)"
koshkit calc "iseven(42)"
koshkit calc "isodd(42)"
koshkit calc "sgn(-0.01)"
'

echo "=== function errors ==="
"$BIN" -c '
koshkit calc "fact(-1)"
koshkit calc "fib(1.5)"
koshkit calc "gcd()"
koshkit calc "cmp(1)"
koshkit calc "min(1, 2, 3,)"
koshkit calc "unknown(1)"
' 2>&1

echo "=== located parse error ==="
"$BIN" -c 'koshkit calc "1 +"' 2>&1

echo "=== located division by zero ==="
"$BIN" -c 'koshkit calc "5 / 0"' 2>&1

echo "=== no expression ==="
"$BIN" -c 'koshkit calc' 2>&1
echo "rc=$?"

unset KOSH_FLAGS
# The calc REPL binds a name=value line as a deferred formula, the right side
# stored unevaluated so it recomputes from the variables it names on each read. A
# standalone assignment prints nothing, an unset variable is an error rather than
# a silent zero, a == comparison stays an expression, and an assignment used
# mid-line is an expression that returns its value.
echo "== a standalone assignment is silent, the read prints the value:"
printf 'x = 2 + 3\nx\nx * 2\n' | "$BIN" -c 'koshkit calc -i' 2>&1
echo "== a formula binds before its inputs and recomputes lazily:"
printf 'area = w * h\nw = 3\nh = 4\narea\nw = 10\narea\n' | "$BIN" -c 'koshkit calc -i' 2>&1
echo "== an assignment is an expression mid-line, returning its value:"
printf '(n = 5) + 1\nn\n' | "$BIN" -c 'koshkit calc -i' 2>&1
echo "== a == comparison is not an assignment:"
printf '5 == 5\n5 == 4\n' | "$BIN" -c 'koshkit calc -i' 2>&1
echo "== an unset variable is a located capitalized error, not a silent zero:"
printf 'nope\n' | "$BIN" -c 'koshkit calc -i' 2>&1
echo "== a set but empty variable reads zero, only an unset name is an error:"
printf 'e\n' | "$BIN" -c 'e=; koshkit calc -i' 2>&1
echo "== an empty assignment is rejected rather than binding an empty value:"
printf 'z =\n' | "$BIN" -c 'koshkit calc -i' 2>&1
echo "== a division by zero is a located capitalized error:"
printf '1 / 0\n' | "$BIN" -c 'koshkit calc -i' 2>&1
echo "== an unset name inside a stored formula errors when the formula reads:"
printf 'total = base + 1\ntotal\n' | "$BIN" -c 'koshkit calc -i' 2>&1

unset KOSH_FLAGS
# calc reports a located error with a caret under the offending token rather than
# a flat message, and with -i it reads expressions interactively, evaluating each
# piped line and continuing past a bad one.
echo "== a decimal divisor preserves decimal arithmetic:"
"$BIN" -c 'koshkit calc 2312312 / 1323.0' 2>&1
echo "== a leading decimal point is accepted:"
"$BIN" -c "koshkit calc '.5 + .25'" 2>&1
"$BIN" -c "koshkit calc -- '-.125 * 8'" 2>&1
echo "== a valid expression prints the value:"
"$BIN" -c "koshkit calc '2 + 3 * 4'" 2>&1
echo "== 128-bit width still prints in full:"
"$BIN" -c "koshkit calc '2 ** 70'" 2>&1
echo "== -i evaluates each piped line, continuing past a bad one:"
printf '1 + 1\nbad +\n10 * 10\n' | "$BIN" -c 'koshkit calc -i' 2>&1

unset KOSH_FLAGS
# calc with -p reads and evaluates expressions from standard input one per line
# with no prompt, a name = value line binds a variable for a later line, and
# with no expression off a pipe it reports a verbose located error.
echo "== -p evaluates each piped line and binds a variable:"
printf '2 + 2\nx = 5\nx * x\n' | "$BIN" -c 'koshkit calc -p' 2>&1
echo "== --pipe is the long form:"
printf '7 * 6\n' | "$BIN" -c 'koshkit calc --pipe' 2>&1
echo "== no expression off a pipe reports a verbose error:"
printf '' | "$BIN" -c 'koshkit calc' 2>&1
