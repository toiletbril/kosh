#!/bin/bash
# read -n reads at most the given number of bytes, and read -u reads from the
# named descriptor rather than the standard input.
printf 'abcdef' | { read -n 3 x; echo "n=[$x]"; }
printf 'hello\n' | { read -n 0 z; echo "n0=rc$? [$z]"; }
file=$(mktemp)
printf 'fromfd\n' > "$file"
exec 7< "$file"
read -u 7 y
echo "u=[$y]"
exec 7<&-
rm -f "$file"


# Bash mapfile and readarray reading lines from a here-string, checked
# byte-for-byte against bash. The element count and individual elements print so
# the comparison stays deterministic.
mapfile -t lines <<< $'alpha\nbeta\ngamma'
echo "count=${#lines[@]}"
echo "first=${lines[0]} last=${lines[2]}"
readarray -t arr <<< $'1\n2\n3\n4'
echo "n=${#arr[@]} edge=$((arr[0]+arr[3]))"
mapfile plain <<< $'p\nq'
echo "plain n=${#plain[@]}"
i=0
while [ $i -lt ${#lines[@]} ]; do
  echo "line $i is ${lines[i]}"
  i=$((i+1))
done

# mapfile -d sets the line delimiter, -s skips leading lines, and -O assigns into
# the existing array from the given index, keeping the elements outside the
# written range.
printf 'a:b:c:' | { mapfile -t -d : arr; echo "d=${arr[2]}-${#arr[@]}"; }
printf 'l1\nl2\nl3\nl4\n' | { mapfile -t -s 2 arr; echo "s=${arr[0]}-${#arr[@]}"; }
printf 'x\ny\n' | { arr=(K0 K1 K2); mapfile -t -O 1 arr; echo "O=${arr[0]}-${arr[1]}-${arr[2]}"; }

printf 'w\nx\ny\nz\n' | { mapfile -t -n 2 v; echo "n=${#v[@]} first=${v[0]} last=${v[1]}"; }
printf 'a\nb\nc\n' | { mapfile -t -n 0 all; echo "all=${#all[@]}"; }
printf '1\n2\n3\n4\n5\n' | { mapfile -t -n3 three; echo "three=${#three[@]} ${three[2]}"; }
printf 'p\nq\n' | { mapfile -t both; echo "both=${#both[@]}"; }
mapfile_callback()
{
  previous_index=$(($1 - 1))
  echo "callback=$1,$2,previous=${callback_rows[$previous_index]-unset}"
  return 7
}
printf 'a\nb\nc\n' | {
  mapfile -t -C mapfile_callback -c 2 callback_rows
  echo "callback-status=$? count=${#callback_rows[@]}"
}
mapfile_origin_callback()
{
  echo "origin=$1,$2,first=${origin_rows[4]-unset}"
}
printf 'x\ny\n' | {
  mapfile -t -O 4 -C mapfile_origin_callback -c 1 origin_rows
  echo "origin-count=${#origin_rows[@]}"
}
printf 'z\n' | mapfile -t -c 0 invalid_quantum 2>/dev/null
echo "callback-zero=$?"

# Bash read -d delimiter, checked byte-for-byte against bash. An empty delimiter
# reads until a NUL byte, so the whole input is slurped and split on IFS, the
# form a bash-completion script uses to load a compgen run into an array. A
# non-empty delimiter stops the read at its first byte.
printf 'a\nb\nc\n' | { IFS=$'\n' read -r -d '' -a arr; echo "${#arr[@]}:${arr[0]}:${arr[2]}"; }
read -r -d : a b <<< 'foo:bar:baz'
echo "[$a][$b]"
printf 'x y z:rest\n' | { read -r -d : first; echo "got=$first"; }
IFS=$'\n' read -r -d '' -a lines < <(printf 'one\ntwo\nthree\n')
echo "${#lines[@]}:${lines[1]}"

tmp=$(mktemp)
printf 'java,kotlin,scala\n' > "$tmp"
echo "direct: $(< "$tmp")"
v=$(< "$tmp")
echo "var: $v"
IFS=','
candidates=($(< "$tmp"))
echo "count: ${#candidates[@]} first: ${candidates[0]}"
rm -f "$tmp"

# Bash read -a and mapfile/readarray, checked byte-for-byte against bash. Input
# arrives by pipe rather than a here-string, and elements are read by index since
# a quoted array view does not yet split per element.
echo "a b c d" | { read -a w; echo "${w[0]} ${w[3]} ${#w[@]}"; }
echo "10 20 30 40 50" | { read -a nums; echo "${nums[2]} ${#nums[@]}"; }
printf 'l1\nl2\nl3\n' | { mapfile lines; echo "${#lines[@]}"; }
printf 'x\ny\nz\n' | { mapfile -t lines; echo "${lines[0]}-${lines[2]} ${#lines[@]}"; }
printf 'apple\nbanana\ncherry\n' | { readarray -t fruit; echo "${fruit[1]} ${#fruit[@]}"; }
printf 'one\ntwo\nthree\n' | { mapfile -t arr; i=0; while [ $i -lt ${#arr[@]} ]; do echo "line $i is ${arr[i]}"; i=$((i+1)); done; }
echo "single" | { read -a one; echo "${#one[@]} ${one[0]}"; }

# Bash read -p prompt, checked byte-for-byte against bash. The prompt goes to
# standard error and only when reading from a terminal, so a piped read shows no
# prompt and stdout carries only the result.
echo "world" | { read -p "name: " x; echo "hello $x"; }
echo "a b c" | { read -p "vals: " -a arr; echo "${arr[0]}-${arr[2]}"; }
printf 'value\n' | { read -r -p "P> " line; echo "[$line]"; }
echo "one two" | { read -p "two: " first second; echo "$second $first"; }

# Without -r the read builtin processes backslashes, checked byte-for-byte
# against bash. A trailing backslash continues the line into the next, a
# backslash before a space escapes the field separator so the space stays in the
# word, a backslash before a plain byte is dropped, and a doubled backslash
# yields one literal backslash.
printf 'a\\\nb c\n' | { read line; echo "[$line]"; }
printf 'first \\\nsecond\n' | { read a b c; echo "a=[$a] b=[$b] c=[$c]"; }
printf 'one\\ two three\n' | { read a b; echo "a=[$a] b=[$b]"; }
printf 'x\\ty\n' | { read a b; echo "a=[$a] b=[$b]"; }
printf 'keep\\\\literal\n' | { read x; echo "x=[$x]"; }
printf 'raw\\ kept\n' | { read -r a b; echo "raw a=[$a] b=[$b]"; }
