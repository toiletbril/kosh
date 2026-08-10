#!/bin/bash
# Bash indexed arrays, assignment a=(x y z) and element access, checked
# byte-for-byte against bash. Covers numeric index, @ and *, the scalar read of
# element zero, a negative index, an arithmetic subscript, out of range, building
# one array from another, and iteration.
a=(x y z)
echo "${a[0]}"
echo "${a[1]}"
echo "${a[2]}"
echo "${a[@]}"
echo "${a[*]}"
echo "$a"
echo "${a[-1]}"
i=1
echo "${a[i]}"
echo "${a[1+1]}"
echo "[${a[9]}]"
b=("${a[@]}" w)
echo "${b[@]}"
fruits=(apple banana cherry)
for f in ${fruits[@]}; do echo "fruit $f"; done
nums=(5 10 15 20)
echo "${nums[3]}"
empty=()
echo "[${empty[@]}]"
mixed=(one "two three" four)
echo "${mixed[1]}"
echo "${mixed[@]}"


# The alternate or default word of an array-subject modifier follows its own
# spelling, so an inner "${b[*]}" joins on the first IFS byte into one field
# while an inner "${b[@]}" keeps one field per element, whatever the subject
# uses. The subject's own [*] still joins when the - form emits the elements.
a=(x y)
b=(p q r)
IFS=-
printf '[%s]\n' ${a[@]+"${b[*]}"}
printf '[%s]\n' "${a[@]+${b[*]}}"
printf '[%s]\n' ${a[@]+"${b[@]}"}
unset c
printf '[%s]\n' ${c[@]-"${b[*]}"}
printf '[%s]\n' "${a[*]-unused}"
set -- ${a[@]+"${b[*]}"}
echo "n=$#"
unset IFS
printf '[%s]\n' ${a[@]+"${b[*]}"}

b=([3]=x [7]=y)
echo "1: vals=[${b[@]}] count=${#b[@]} idx=[${!b[@]}]"
c=(a b [5]=f g)
echo "2: vals=[${c[@]}] idx=[${!c[@]}]"
d=([2]=two [0]=zero [1]=one)
echo "3: vals=[${d[@]}] idx=[${!d[@]}] d0=${d[0]}"
e=(plain [10]=ten more)
echo "4: idx=[${!e[@]}] ten=${e[10]} more=${e[11]}"

fruits=(apple banana cherry)
for i in "${!fruits[@]}"; do echo "idx $i = ${fruits[$i]}"; done
echo "joined: ${!fruits[*]}"
declare -A m=([x]=1 [y]=2)
for k in "${!m[@]}"; do echo "key $k"; done | sort
count=0
for i in "${!fruits[@]}"; do count=$((count+1)); done
echo "loopcount=$count"

# Bash whole-array copy and merge, checked byte-for-byte against bash. A quoted
# at copy duplicates every element so a later write to the copy leaves the
# original alone, a command substitution that field splits its lines builds a new
# array, and a merge of a copy with an extra element grows the count by one.
a=(one "two three" four five)
b=("${a[@]}")
echo "copy-count: ${#b[@]}"
printf '<%s>' "${b[@]}"; echo
b[0]=ONE
echo "orig-unchanged: ${a[0]}"
echo "copy-changed: ${b[0]}"
nums=(3 1 2)
sorted=($(printf '%s\n' "${nums[@]}" | sort))
echo "sorted: ${sorted[@]}"
merged=("${a[@]}" extra)
echo "merged-count: ${#merged[@]} last=${merged[-1]}"

a=(alpha beta gamma delta epsilon)
echo "1: ${a[@]:1:2}"
echo "2: ${a[@]:2}"
echo "3: ${a[*]:0:3}"
echo "4: [${a[@]: -2}]"
echo "5: [${a[@]: -2:1}]"
echo "count: $(set -- ${a[@]:1:3}; echo $#)"
for x in "${a[@]:1:2}"; do echo "elem=$x"; done
b=(one two three)
echo "neg-len: ${b[@]:0:-1}"

# Bash array length forms, checked byte-for-byte against bash. The at and star
# counts report the element total, an element length reports its string length,
# and append grows the count across single and multiple element appends starting
# from a populated array and from an empty one.
a=(one two three four)
echo "at-len: ${#a[@]}"
echo "star-len: ${#a[*]}"
echo "e0: ${#a[0]}"
echo "e2: ${#a[2]}"
a+=(five)
echo "after-append: ${#a[@]} last=${a[4]}"
a+=(six seven)
echo "multi-append: ${#a[@]} ${a[@]}"
b=()
echo "empty-len: ${#b[@]}"
b+=(z)
echo "grown: ${#b[@]} ${b[0]}"
c=(x)
c+=(y z)
echo "c: ${c[@]} count=${#c[@]}"

# Bash array element assignment a[i]=v, append a+=(...), count ${#a[@]}, and
# index list ${!a[@]}, checked byte-for-byte against bash on contiguous arrays.
a=(x y z)
echo "${#a[@]}"
echo "${!a[@]}"
a[1]=Y
echo "${a[@]}"
a+=(w v)
echo "${a[@]}"
echo "${#a[@]}"
b=(1 2 3)
b[1]+=0
echo "${b[@]}"
c=(p q r)
i=2
c[i]=Z
echo "${c[@]}"
c[-1]=last
echo "${c[@]}"
arr=(a b c)
arr+=(d)
arr[0]=A
echo "${arr[@]}"
echo "count=${#arr[@]} indices=${!arr[@]}"
greet=(hello world)
echo "${#greet[1]}"

# The ${name+"${name[@]}"} idiom with a bare scalar subject keeps one field per
# element with the empty ones intact, the construct bash-completion's
# _comp_get_words passes to _comp_upvars. The bare name reads as element zero,
# so the plain form tests existence and the colon form the first element.
w=(a "")
set -- ${w+"${w[@]}"}
echo "set_with_trailing_empty=$#"
w=("" b)
set -- ${w+"${w[@]}"}
echo "set_with_leading_empty=$#"
w=()
set -- ${w+"${w[@]}"}
echo "empty_array=$#"
unset w
set -- ${w+"${w[@]}"}
echo "unset_array=$#"
w=""
set -- ${w:+"${w[@]}"}
echo "colon_empty_scalar=$#"
unset w
q=(1 2)
set -- ${w-"${q[@]}"}
echo "dash_unset_subject=$#"
w=x
set -- ${w-"${q[@]}"}
echo "dash_set_subject=$#-$1"
f() { echo "args=$#"; for a in "$@"; do printf '[%s]' "$a"; done; echo; }
words=(bash "")
f -a"${#words[@]}" words ${words[@]+"${words[@]}"}
f -a"${#words[@]}" words ${words+"${words[@]}"}

# Bash array quoting forms, checked byte-for-byte against bash. A quoted at view
# keeps each element a separate word, a quoted star view joins on the first IFS
# byte, an unquoted at view field splits, and a single-element star copy lands as
# one element while a quoted at copy keeps the element count.
a=(one "two three" four)
printf '[%s]' "${a[@]}"; echo
printf '[%s]' "${a[*]}"; echo
printf '[%s]' ${a[@]}; echo
IFS=-
printf '[%s]' "${a[*]}"; echo
unset IFS
b=("${a[@]}")
printf '<%s>' "${b[@]}"; echo
echo "count=${#b[@]}"
c=("${a[*]}")
echo "starcopy=${#c[@]} first=[${c[0]}]"

# Unset of a dense array element leaves a hole rather than renumbering the later
# indices, so the surviving indices, the values, and a re-assignment into the
# hole all match bash.
a=(a b c d)
unset 'a[1]'
echo "${!a[@]}"
echo "${a[@]}"
echo "${a[2]}"
a[1]=X
echo "${!a[@]}"
echo "${a[@]}"
unset 'a[0]'
echo "${!a[@]}"
echo "${a[@]}"

arr=("a b" c)
printf '[%s]' ${arr[@]+"${arr[@]}"}; echo
empty=()
printf '<%s>' ${empty[@]+"${empty[@]}"}; echo "(done)"
for x in ${arr[@]+"${arr[@]}"}; do echo "iter=$x"; done

a=(a b c d e)
unset 'a[2]'
echo "vals=${a[@]} count=${#a[@]}"
declare -A cap=([france]=paris [japan]=tokyo [italy]=rome)
unset 'cap[france]'
echo "capcount=${#cap[@]} japan=${cap[japan]} france=[${cap[france]}]"
b=(x y z)
unset 'b[-1]'
echo "b=${b[@]}"

# unset removes a sparse array element held in the sparse map, not only a dense
# one, so the index disappears from the key list and the value list shrinks.
a=([0]=x [5]=y [10]=z)
echo "${!a[@]}"
echo "${a[@]}"
unset 'a[5]'
echo "${!a[@]}"
echo "${a[@]}"
unset 'a[10]'
echo "${!a[@]}"
echo "${a[@]}"

a=(x y z)
a[5]=q
echo "at5: vals=[${a[@]}] count=${#a[@]} idx=[${!a[@]}]"
b=([3]=x [7]=y)
echo "init: vals=[${b[@]}] count=${#b[@]} idx=[${!b[@]}]"
c=()
c[10]=hello
echo "single: vals=[${c[@]}] count=${#c[@]} idx=[${!c[@]}]"

# An array's setness is element zero's, and the test, assign, and substring
# modifiers run against one element with its own setness.
a=(); echo "[${a+y}]"
b=(x); echo "[${b+y}]"
declare -a c=([1]=q); echo "[${c+y}]" "[${c[1]+y}]" "[${c[0]+y}]"
e=(abcdef ghij)
echo "[${e[1]:1}]" "[${e[1]:1:2}]" "[${e[9]:-fb}]" "[${e[0]:+plus}]"
echo "[${e[9]=zz}]" "[${e[9]}]"
declare -A m=([k]=v)
echo "[${m[k]+y}]" "[${m[no]+y}]" "[${m[no]:-fb2}]"

a=(hello world)
echo "elem: ${a[0]//l/L}"
b=(aXa bXb cXc)
printf '[%s]' "${b[@]//X/-}"; echo
echo "star: ${b[*]//X/+}"
d=(local -A NAME)
g() { "${d[@]//NAME/myassoc}"; myassoc[key/with/slash]=v; echo "got: ${myassoc[key/with/slash]}"; }
g

a=(apple banana cherry)
echo "${a[@]#a}"
echo "${a[@]%y}"
echo "${a[@]^^}"
echo "${a[@],,}"
echo "${a[*]^^}"
echo "${a[*],,}"
echo "${a[1]^^}"
echo "${a[0]#ap}"
echo "${a[2]/r/R}"
echo "${a[2]//r/R}"
echo "${a[@]##*a}"
echo "${a[@]%%n*}"
b=(Foo BAR baz)
echo "${b[@],}"
echo "${b[@]^}"
echo "${b[1],,}"
echo "${b[2]^}"

declare -A v
k=./some/path
v[$k]=1
echo "assoc: ${v[$k]}"
a=(0 0 0)
i=2
a[$i]=9
echo "indexed: ${a[@]}"
a[$i]+=1
echo "append: ${a[2]}"
declare -A h
h[$((1+1))]=z
echo "arith: ${h[2]}"
key="x y"
declare -A m
m[$key]=found
echo "spaced: ${m[$key]}"

unset a
echo "[${a[@]-default}]"
b=(1 2 3)
echo "[${b[@]+alt}]"
echo "[${b[@]-elems}]"
unset c
echo "[${c[@]+set}]"
d=(100 200)
sparse_check() { local d; d[5]=local; }
d[40]=keep
sparse_check
echo "[${d[40]}]"

# Bash iteration over an array, checked byte-for-byte against bash. A quoted at
# view yields one loop word per element so an element with a space stays whole,
# an unquoted at view field splits each element, and arithmetic over the elements
# accumulates the running total.
fruits=(apple banana cherry)
for f in "${fruits[@]}"; do echo "fruit: $f"; done
n=(5 10 15 20)
total=0
for v in "${n[@]}"; do total=$((total+v)); done
echo "sum: $total"
words=("hello world" foo bar)
for w in "${words[@]}"; do echo "word=[$w]"; done
count=0
for w in ${words[@]}; do count=$((count+1)); done
echo "unquoted-split-count: $count"
for i in ${!fruits[@]}; do echo "$i -> ${fruits[$i]}"; done

# The @op transform maps over each array element and positional parameter,
# checked byte-for-byte against bash. The per-element value transforms Q, U, L,
# u, and the a attribute map, the star form joins under the first IFS byte, and
# the at form keeps each element its own field.
a=(a "b c" d)
printf '<%s>' "${a[@]@Q}"; echo
echo "${a[*]@Q}"
upper=(x Y z)
echo "${upper[@]@U}"
echo "${upper[@]@L}"
echo "${upper[@]@u}"
echo "${a[@]@a}"
ra=(1 2); readonly ra
echo "${ra[@]@a}"
set -- one "two three" four
printf '<%s>' "${@@Q}"; echo
echo "${@@U}"
echo "${*@u}"

a=() b=(); echo "empties: ${#a[@]} ${#b[@]}"
flags= pvars=() specs=(); echo "mixed: [$flags] ${#pvars[@]} ${#specs[@]}"
x=1 y=(p q r); echo "scalar-then-array: $x ${y[2]}"
p=(1 2) q=() r=(9); echo "three: ${p[1]} ${#q[@]} ${r[0]}"

a[100000000]=far
echo "far: ${a[100000000]}"
a[5]=near
echo "mixed: ${a[5]}-${a[100000000]}"
k[63|67108864]=1
echo "mask: ${k[67108927]}"
echo "unset-far: [${a[999999999]}]"
b[268435456]=x
b[268435457]=y
echo "two-sparse: ${b[268435456]}${b[268435457]}"
c=()
c[5]=five
c[10]=ten
echo "sparse-count: ${#c[@]} last=${c[-1]}"
unset 'c[5]'
echo "sparse-count-after-unset: ${#c[@]}"

scalar_indexed=value
declare -a scalar_indexed
declare -p scalar_indexed
scalar_associative=value
declare -A scalar_associative
declare -p scalar_associative
conversion_indexed=(one two)
declare -A conversion_indexed 2>/dev/null
echo "indexed-conversion=$? value=${conversion_indexed[*]}"
declare -A conversion_associative=([key]=value)
declare -a conversion_associative 2>/dev/null
echo "associative-conversion=$? value=${conversion_associative[key]}"

declare -A unset_by_variable=([foo]=bar)
unset_key=foo
unset 'unset_by_variable[$unset_key]'
echo "assoc-unset-expanded=[${unset_by_variable[foo]+set}]"
declare -A unset_spaced=(["a b"]=value)
unset_spaced_key="a b"
unset 'unset_spaced[$unset_spaced_key]'
echo "assoc-unset-spaced=[${unset_spaced["a b"]+set}]"
declare -A unset_once=([foo]=value)
unset 'unset_once[$(printf hit >&2; printf foo)]'
echo "assoc-unset-once=[${unset_once[foo]+set}]"
declare -aA invalid_both 2>/dev/null
printf 'declare-both=%s\n' "$?"
