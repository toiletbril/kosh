#!/bin/bash
# Bash (( expr )) arithmetic command, checked byte-for-byte against bash. The
# status is success when the value is non-zero. Covers comparison, assignment,
# compound assignment, pre and post increment, and nested parentheses.
(( 1 + 1 )) && echo a
(( 0 )) || echo b
(( 5 > 3 )) && echo c
(( 2 * (3 + 4) == 14 )) && echo d
x=5
(( x++ ))
echo "$x"
(( ++x ))
echo "$x"
y=10
(( y -= 4 ))
echo "$y"
z=0
(( z = 3 * 3 ))
echo "$z"
n=10
(( n % 3 )) && echo e
(( 1 == 1 && 2 == 2 )) && echo f
(( 1 == 2 || 3 == 3 )) && echo g
echo "$(( 6 / 2 + 1 ))"
i=5
echo "$(( i++ ))"
echo "$i"
echo "$(( ++i ))"
count=0
while (( count < 3 )); do
  echo "loop $count"
  (( count++ ))
done


# Bash unary operators and grouping in $(( )), checked byte-for-byte against
# bash. Covers unary minus and plus, double negation, deeply nested
# parentheses, and the precedence override grouping forces.
echo "neg $(( -5 )) $(( -5 + -3 )) $(( - -5 ))"
echo "pos $(( +5 )) $(( +-5 ))"
echo "group $(( -(3 + 4) )) $(( -(2 * 3) + 1 ))"
echo "deep $(( ((((1 + 2)) * 3)) - ((4)) ))"
echo "over $(( 2 * (3 + 4) )) $(( (2 + 3) * (4 - 1) ))"
echo "divmod $(( 17 / 5 )) $(( 17 % 5 )) $(( -17 / 5 )) $(( -17 % 5 ))"

# A variable read inside arithmetic is re-evaluated as an arithmetic
# expression, the same as bash. A value with surrounding blanks, an operator,
# an indirect name, a sign, or a hex form all resolve, and an array subscript
# reads the same way.
x=" 5 "; echo $((x + 1))
y="2*3"; echo $((y))
a=c; c=7; echo $((a))
n=-4; echo $((n * 2))
h=0x10; echo $((h + 1))
w="  7  "; echo $((w))
z=" 3 "; arr=(0 10 20 30); echo "${arr[z]}"

echo "dec: $((10#15))"
echo "hex: $((16#ff))"
echo "bin: $((2#101))"
echo "octbase: $((8#17))"
echo "b36: $((36#z))"
echo "lead0: $((10#0042))"
echo "mixed: $((16#A + 2#10))"
u=42.9
echo "trunc: $((10#0${u%.*}))"

# Bash logical and comparison arithmetic operators inside $(( )), checked
# byte-for-byte against bash. Covers the six comparisons, logical and, or, not,
# and short-circuit evaluation that leaves a side effect unrun.
echo "lt $(( 3 < 5 )) $(( 5 < 5 ))"
echo "le $(( 5 <= 5 )) $(( 6 <= 5 ))"
echo "gt $(( 6 > 2 )) $(( 2 > 6 ))"
echo "ge $(( 2 >= 9 )) $(( 9 >= 9 ))"
echo "eq $(( 4 == 4 )) $(( 4 == 5 ))"
echo "ne $(( 4 != 4 )) $(( 4 != 5 ))"
echo "and $(( 1 && 1 )) $(( 1 && 0 )) $(( 0 && 1 ))"
echo "or $(( 0 || 1 )) $(( 0 || 0 )) $(( 1 || 0 ))"
echo "not $(( !0 )) $(( !7 )) $(( !!7 ))"
x=5
(( 0 && (x = 99) ))
echo "shortand $x"
(( 1 || (x = 77) ))
echo "shortor $x"

echo "empty: $(( ))"
n=(a b c)
echo "unset-sub: ${n[$1]}"
echo "expr-empty: $(( ${#nosuchvar} ))"
type -at if
f() { :; }
type -at f
alias_target_builtin() { :; }
type -t type

# A (( that closes with a lone parenthesis is a subshell whose first child is
# a subshell, while a true arithmetic command keeps the (( reading, the
# disambiguation arith.tests runs at its line 191.
((echo abc; echo def;); echo ghi)
echo "subshell=$?"
((1+2)); echo "arith_true=$?"
((0)); echo "arith_false=$?"
(( (3>2) && (2>1) )); echo "nested=$?"
x=$(( (1+2) * 2 )); echo "x=$x"

a=(5 2 0)
echo "read: $((a[0]*100+a[1]))"
i=2
echo "var-index: $((a[i]+a[i-1]))"
a[0]=9
echo "write: $((a[0]))"
(( a[1]++ ))
echo "postinc: ${a[1]}"
declare -A m
m[k]=10
echo "assoc: $((m[k]+5))"
n=$(( a[0] + a[1] ))
echo "captured: $n"

counter=0
expression='counter++'
echo "recursive: $((expression-expression)) counter=$counter"
cached=9
echo "cached-before: $((cached))"
unset cached
echo "cached-after: $((cached))"

# Bash bitwise arithmetic operators inside $(( )), checked byte-for-byte against
# bash. Covers complement, and, or, xor, and the two shifts, with negative
# operands and a precedence mix.
echo "not $(( ~0 )) $(( ~5 )) $(( ~-1 ))"
echo "and $(( 12 & 10 )) $(( 255 & 15 ))"
echo "or $(( 12 | 1 )) $(( 8 | 4 | 2 | 1 ))"
echo "xor $(( 12 ^ 6 )) $(( 5 ^ 5 ))"
echo "shl $(( 1 << 8 )) $(( 3 << 4 ))"
echo "shr $(( 256 >> 3 )) $(( -8 >> 1 ))"
echo "prec $(( 1 << 4 | 2 & 3 ^ 5 ))"
echo "mask $(( (1 << 5) - 1 ))"

# Bash ternary and comma arithmetic operators inside $(( )), checked
# byte-for-byte against bash. Covers a plain ternary, a nested ternary, the
# comma operator yielding its last value, and a comma sequence with side
# effects.
echo "tern $(( 5 > 3 ? 10 : 20 )) $(( 5 < 3 ? 10 : 20 ))"
echo "nest $(( 1 ? 2 ? 30 : 40 : 50 )) $(( 0 ? 2 : 3 ? 60 : 70 ))"
echo "comma $(( 1 + 1, 2 + 2, 3 + 3 ))"
a=0; b=0
echo "seq $(( a = 5, b = a * 2, a + b ))"
echo "after $a $b"
n=2
echo "ternassign $(( n > 0 ? (n = 100) : (n = -100) ))"
echo "afterternary $n"

# Bash arithmetic assignment operators inside (( )), checked byte-for-byte
# against bash. Covers plain assignment and every compound form, the value the
# assignment yields, and chained assignment.
n=5
(( n = 100 ))
echo "set $n"
(( n += 7 )); echo "add $n"
(( n -= 12 )); echo "sub $n"
(( n *= 3 )); echo "mul $n"
(( n /= 4 )); echo "div $n"
(( n %= 50 )); echo "mod $n"
(( n <<= 4 )); echo "shl $n"
(( n >>= 2 )); echo "shr $n"
(( n &= 30 )); echo "and $n"
(( n |= 1 )); echo "or $n"
(( n ^= 6 )); echo "xor $n"
echo "yield $(( n += 10 ))"
a=1; b=2; c=3
(( a = b = c = 9 ))
echo "chain $a $b $c"

echo $(( 2 ** 10 ))
echo $(( -2 ** 2 ))
echo $(( 2 ** 3 ** 2 ))
echo $(( 3 * 2 ** 3 ))
echo $(( 2 ** 0 ))
echo $(( (1 + 1) ** 4 ))

# Bash numeric literal bases in $(( )), checked byte-for-byte against bash.
# Covers the C hex and octal prefixes, the explicit base#digits form for several
# bases including base 64, and the digit set base 64 uses past the alphabet.
echo "hex $(( 0xff )) $(( 0XfF )) $(( 0x10 ))"
echo "oct $(( 010 )) $(( 0777 ))"
echo "based $(( 2#101 )) $(( 8#17 )) $(( 16#FF )) $(( 10#0042 ))"
echo "b36 $(( 36#z )) $(( 36#10 ))"
echo "b64 $(( 64#A )) $(( 64#a )) $(( 64#0 )) $(( 64#9 )) $(( 64#_ )) $(( 64#@ ))"
echo "mix $(( 16#A + 2#10 + 010 ))"

# Bash let builtin arithmetic forms, checked byte-for-byte against bash. Covers a
# parenthesized expression, the assignment operators through let, increment and
# decrement, a multi-argument let where a later argument reads an earlier
# assignment, and the let exit status that follows the last argument value.
let "v = (3 + 4) * 2 - 1"
echo "v $v"
let w=10 "w += 5" "w *= 2"
echo "w $w"
let p=3 q=4 "r = p * q"
echo "r $r"
i=5
let i++
let ++i
let i--
echo "i $i"
let "z = 7" "z <<= 2"
echo "z $z"
let "0"; echo "rc-zero $?"
let "9"; echo "rc-nonzero $?"

# Bash (( )) arithmetic command exit status, checked byte-for-byte against bash.
# The command succeeds when the value is non-zero and fails when it is zero, so a
# negative value also succeeds. Covers the status in $?, in an if, and through
# the && and || connectives.
(( 5 )); echo "five rc=$?"
(( 0 )); echo "zero rc=$?"
(( -3 )); echo "neg rc=$?"
(( 2 - 2 )); echo "cancel rc=$?"
if (( 2 + 2 == 4 )); then echo "if true"; fi
if (( 1 == 2 )); then echo nope; else echo "if false"; fi
(( 3 > 1 )) && echo "and true"
(( 3 < 1 )) || echo "or false"
total=0
i=0
while (( i < 4 )); do
  (( total += i ))
  (( i++ ))
done
echo "total $total"

let "x = 5 + 3"
echo "x=$x"
let a=0; echo "zero rc=$?"
let b=7; echo "nonzero rc=$?"
i=5; let i++; let ++i
echo "steps i=$i"
let "p = 2" "q = p * 3 + 1"
echo "chain p=$p q=$q"
let "r = (1 << 4) | 2"
echo "bitwise r=$r"
