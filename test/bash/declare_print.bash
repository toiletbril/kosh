#!/bin/bash
# Bash declare -p, checked byte-for-byte against bash. Prints the declaration of
# a scalar, an indexed array, an empty array, and a single-key associative array,
# plus the error for an unset name. Multi-key associative order is store order,
# not bash order, so the test uses one key.
x=5
declare -p x
greeting=hello
declare -p greeting
a=(one two three)
declare -p a
empty=()
declare -p empty
declare -A m
m[only]=value
declare -p m
declare -p definitely_unset 2>/dev/null
echo "unset rc=$?"
nums=(10 20 30 40)
declare -p nums


# The += append form on declare, local, and export concatenates onto the current
# value rather than dropping the operator, while a += on a plain command word
# stays literal.
declare d=foo
declare d+=bar
echo "$d"
export E=ab
export E+=cd
echo "$E"
f() { local p=xy; local p+=z; echo "$p"; }
f
echo k+=v

# The integer mark is scoped with a local binding, applies to array elements,
# and declare -p prints it for arrays and for a name that has no value yet.
declare -i g=5
f() { local g; g=2+2; echo "plain_local_suppresses=[$g]"; }
f
echo "outer_keeps_mark=$g"
h() { local -i x=5; x+=3; echo "local_i_adds=$x"; }
h
x=1+1
echo "mark_gone_after_return=[$x]"
m() { local -ia nums; nums[0]=2; nums[0]+=3; echo "local_int_array=${nums[0]}"; }
m
declare -ia arr
arr[0]=5
arr[0]+=3
echo "indexed_element_adds=${arr[0]}"
declare -iA map
map[k]=5
map[k]+=3
echo "assoc_element_adds=${map[k]}"
declare -ia plain
plain[0]="2+3"
echo "plain_element_evaluates=${plain[0]}"
declare -i b[0]=7
b[0]+=1
echo "subscripted_declare=${b[0]}"
declare -p b
declare -i z
declare -p z
declare -iA m3
m3[q]=5
declare -p m3
declare -ix q2=5
q2+=3 env > /tmp/kosh_test_q2env_$$ 2>/dev/null
grep "^q2=" /tmp/kosh_test_q2env_$$
echo "prefix_restores=$q2"
rm -f /tmp/kosh_test_q2env_$$

# declare -i marks a name so every assignment evaluates as arithmetic, += adds
# rather than concatenates, +i and unset clear the mark, and -p prints it.
declare -i x=5
x+=3
echo "add=$x"
declare -i y="5+5"
echo "expr=$y"
v=5
declare -i v+=3
echo "same_command=$v"
declare -i u=4
declare +i u
u+=3
echo "unmarked=$u"
declare -i t=7
unset t
t=1+1
echo "unset_clears=$t"
declare -i s=2
declare -p s
declare -ix r=3
declare -p r
env | grep '^r='
declare -i q=5
q+="2,3"
echo "comma=$q"
declare -i p=5
p+=
echo "empty=[$p]"
declare -i o
o=abc
echo "invalid=[$o]"
declare -i n=5
export n+=3
echo "export=$n"
env | grep '^n='
typeset -i m=3
m+=4
echo "typeset=$m"

# declare -F answers function existence by status and prints bare names, the
# bare form lists declare -f lines, and a cloned definition evals back.
f() { echo body; }
function g/h:i { echo colon; }
declare -F f; echo "s=$?"
declare -F nope 2>/dev/null; echo "missing=$?"
declare -F f nope 2>/dev/null; echo "multi=$?"
declare -F -- g/h:i >/dev/null; echo "bleform=$?"
declare -F | grep -c 'declare -f f'
typeset -F f
def=$(declare -f g/h:i)
new=${def/#"g/h:i"/"clone:x"}
eval "$new"
clone:x
echo "clone=$?"
declare -f nope2 2>/dev/null; echo "deff_missing=$?"

# A local -r marks the name read-only only for its own scope, so calling the
# function a second time redeclares and reassigns it rather than failing as
# read-only, the way sdkman's completion declares version_paths.
f() {
	local -r vp=("a" "b" "c")
	echo "vp=${vp[1]} count=${#vp[@]}"
}
f
f
g() {
	local -r name="val"
	echo "name=$name"
}
g
g

# local NAME=$value does not word-split or glob the value, the way a plain
# assignment does not, so a value with spaces or a glob character stays one word.
f() { local x=$1; echo "[$x]"; }
f "a b c"
g() { local y=$1; echo "[$y]"; }
g "*"

# Bash local with attribute flags, checked byte-for-byte against bash. -a makes a
# local indexed array, -A an associative one, and -i, -r, -x are accepted. The
# inline array-literal form local -a a=(...) is not supported, only the separate
# assignment.
f() { local -a arr; arr=(x y z); echo "${arr[1]} count=${#arr[@]}"; }
f
g() { local -i n=5; echo "n is $n"; }
g
h() { local -A m; m[key]=value; echo "${m[key]}"; }
h
j() { local x=hi; echo "scalar $x"; }
j
m() { local -r c=constant; echo "readonly $c"; }
m
n() { local y; y=assigned; echo "$y"; }
n
indexed() { local -a inner; inner=(a b c); echo "first ${inner[0]} last ${inner[2]} size ${#inner[@]}"; }
indexed

# A local += on a name that shadows an outer one starts from empty the way bash
# localizes it fresh, while a re-declared local in the same scope appends to its
# own value, and the outer name is restored when the function returns.
x=glob
f() { local x+=ADD; echo "[$x]"; }
f
echo "[$x]"
g() { local y=a; local y+=b; echo "[$y]"; }
g

# Array assignments given as arguments to the assignment builtins local and
# declare, checked byte-for-byte against bash. The elements field split and glob
# the way a command's arguments do, a local array does not leak to the caller,
# and a local array shadows then restores the caller's array.
f() {
  local -r editor=(a b c)
  printf '<%s>' "${editor[@]}"
  echo
  local versions=($(echo one two three))
  echo "${#versions[@]} ${versions[1]}"
}
f
echo "after f: [${editor[@]}]"
outer=(x y z)
g() { local outer=(inner); echo "in g: ${outer[@]}"; }
g
echo "after g: ${outer[@]}"
declare top=(p q r)
echo "${top[@]}"

# export and readonly list their variables in the declare form bash reloads, and
# export -n removes the export attribute while keeping the variable in the shell.
# The listing is filtered to the test's own names so the inherited environment
# cannot perturb the comparison.
export EV=one
echo "export_p=$(export -p | grep '^declare -x EV=')"
echo "export_bare=$(export | grep '^declare -x EV=')"
export -n EV=ignored
echo "unmark_env=$(env | grep -c '^EV=')"
echo "unmark_shell=$EV"
readonly RV=two
echo "readonly_p=$(readonly -p | grep '^declare -r RV=')"

# Bash local inside a function shadowing an outer variable then restoring it on
# return, checked byte-for-byte against bash. A callee that does not redeclare
# the name sees the dynamic-scope value of the caller.
v=outer
show() { echo "in show: $v"; }
mid() { local v=inner; echo "in mid: $v"; show; }
echo "before: $v"
mid
echo "after: $v"
counter() { local v=$1; echo "counter sees $v"; }
counter 99
echo "still: $v"

# In the bash mood a prefix assignment before a special builtin is dropped after
# the command, unlike the POSIX persistence, checked byte-for-byte against bash.
# An exported assignment from the prefix still reaches the environment for the
# command's duration, so the colon's exported name survives while the prefix
# name does not.
x=before
x=after :
echo "colon=[$x]"
y=before
y=after export z=hi
echo "export=[$y][$z]"
w=before
w=after eval ':'
echo "eval=[$w]"
