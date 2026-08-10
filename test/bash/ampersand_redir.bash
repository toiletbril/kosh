#!/bin/bash
# Bash &>, &>>, and |& redirections, checked byte-for-byte against bash. The
# &> forms write to a temp file that is read back, the |& form pipes both
# standard streams so the result is observable on standard output.
tmp=/tmp/kosh_bashdiff_ampredir_$$
echo hello &>"$tmp"
cat "$tmp"
{ echo out; echo err >&2; } &>"$tmp"
sort "$tmp"
echo first >"$tmp"
echo second &>>"$tmp"
sort "$tmp"
rm -f "$tmp"
{ echo o; echo e >&2; } |& sort
ls /nonexistent_path_xyz |& grep -o "No such"
echo piped |& cat
printf 'x\ny\nz\n' |& wc -l | tr -d ' '


# Dynamic file descriptor allocation, the exec {var}>file form and its dup,
# read, close, and compound spellings, checked byte for byte against bash. The
# allocated number depends on the descriptors the harness leaves open, so the
# checks assert the descriptor lands at or above ten and the data flows rather
# than the absolute number, and a brace word with no adjacent redirect stays an
# argument.
tmp=/tmp/kosh_bashdiff_fdalloc_$$

exec {w}>"$tmp"
echo "w>=10: $(( w >= 10 ))"
printf 'line one\n' >&$w
printf 'line two\n' >&$w
exec {w}>&-
cat "$tmp"

exec {dup}>&1
echo "dup>=10: $(( dup >= 10 ))"
printf 'through the dup\n' >&$dup
exec {dup}>&-

printf 'a\nb\n' >"$tmp"
exec {r}<"$tmp"
read first <&$r
read second <&$r
echo "read $first $second"
exec {r}<&-

exec {one}>/dev/null
exec {two}>/dev/null
echo "two distinct: $(( one != two && one >= 10 && two >= 10 ))"
exec {one}>&-
exec {two}>&-

{ echo grouped; } {g}>"$tmp"
echo "g>=10: $(( g >= 10 ))"
cat "$tmp"

echo literal {brace} stays a word

rm -f "$tmp"

# Bash here-string <<<, checked byte-for-byte against bash. Feeds the expanded
# word plus a newline as standard input. Covers a literal, a variable, an empty
# string, piping into a builtin, and a read into a variable.
cat <<< "hello world"
wc -c <<< "abc" | tr -d ' '
v=expanded
cat <<< "$v"
read first rest <<< "one two three"
echo "$first | $rest"
grep -o match <<< "a match here"
rev <<< "stressed"
tr 'a-z' 'A-Z' <<< "lower"
n=42
cat <<< "the number is $n"
while read line; do echo "line: $line"; done <<< "only one"
cat <<< ""
wc -l <<< "no newline added beyond one" | tr -d ' '

# The csh both-streams spelling cmd >&file redirects stdout and stderr to the
# file when the word names no descriptor, while a numeric word keeps the
# descriptor duplication and an explicit fd keeps the strict reading.
out=$(mktemp)
echo visible >&"$out"
ls /nonexistent_zzqq >>"$out" 2>&1
grep -c visible "$out"
grep -c nonexistent "$out"
echo hi >&/dev/null
echo after
exec 3>&1
echo fd3 >&3
exec 3>&-
rm -f "$out"

# A redirection that cannot open its target fails the command with status 1 in
# the bash mood and the shell continues to the next command, checked against
# bash. The POSIX mood reports the same failure as status 2, covered under the
# dash comparison.
cat < /no/such/directory/file 2>/dev/null
echo "read_status=$?"
echo > /no/such/directory/file 2>/dev/null
echo "write_status=$?"
echo still_running
