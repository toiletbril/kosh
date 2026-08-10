#!/bin/sh
# Redirection, descriptor duplication, and pipelines together.

tmp=/tmp/kosh_everything_redir_$$

echo first >"$tmp"
echo second >>"$tmp"
echo "file has:"
cat "$tmp"
echo "line count: $(wc -l <"$tmp")"

sort "$tmp" >"$tmp.sorted"
echo "sorted:"
cat "$tmp.sorted"

errors=$(sh -c 'echo good; echo bad >&2' 2>&1 | sort)
echo "merged: $errors"

count=$(ls /no_such_path_here 2>&1 | grep -c .)
if [ "$count" -ge 1 ]; then
    echo "captured an error line"
fi

ls /no_such_path_here 2>/dev/null
echo "after silenced error"

rm -f "$tmp" "$tmp.sorted"


# The >| noclobber override and the <> read-write redirection operators, checked
# against dash. A real pipe after a file redirection stays a pipe, so the
# operators do not capture an unrelated bar or greater.

f=/tmp/kosh_redirops_$$
rm -f "$f"

# >| writes even when noclobber would refuse a plain >.
set -C
echo first > "$f"
echo second >| "$f"
cat "$f"
set +C

# <> opens for reading and writing without truncating, so a short write leaves
# the rest of the file intact.
printf 'ABCDEF' > "$f"
exec 3<> "$f"
printf 'xy' >&3
exec 3>&-
cat "$f"
echo

# A bar that does not touch the greater is still a pipe stage.
echo piped > "$f" | cat
cat "$f"

rm -f "$f"

# Command substitution, positional parameters, redirection, and pipelines.

set -- one two three four
echo "count=$#"
echo "first=$1 third=$3"
shift
echo "after shift: $1 $#"
shift 2
echo "after shift 2: $1 $#"

result=$(echo "  spaced  " | cat)
echo "[$result]"

tmp=/tmp/kosh_everything_$$
printf '%s\n' gamma alpha beta > "$tmp"
echo "sorted:"
sort "$tmp"
echo "lines: $(wc -l <"$tmp")"
rm -f "$tmp"

count=0
for item in $(echo a b c d); do
    count=$((count + 1))
done
echo "iterated $count items"

echo "pipeline: $(printf '5\n3\n9\n1\n' | sort -n | head -n1)"

i=1
until [ "$i" -gt 3 ]; do
    echo "until $i"
    i=$((i + 1))
done

# A compound command as a pipeline stage, checked against dash.

# A subshell or a brace group feeds the next stage.
(echo hi) | cat
echo x | { cat; }
{ echo grouped; } | cat

# A subshell that prints several lines pipes its whole output.
(echo a; echo b) | wc -l

# A while-read on the right of a pipe consumes the producer.
printf 'a\nb\nc\n' | while read l; do echo "got $l"; done

# A brace group reads several lines from the pipe.
seq 1 3 | { read a; read b; echo "$a-$b"; }

# The stage runs in a subshell, so its variable changes do not escape.
x=1
printf 'one\ntwo\n' | while read l; do x=$l; done
echo "x=$x"

# The pipeline status is the last stage's status.
(echo a) | false
echo "status=$?"

# A negated pipeline with a compound stage inverts the status.
! (echo a) | grep -q zzz
echo "negated=$?"

# A compound stage in the middle of a longer pipeline still wires through.
seq 1 5 | { while read n; do echo "n=$n"; done; } | wc -l

# A downstream stage that exits early frees a compound or function producer,
# so a large producer behind a subshell or a function does not deadlock.
seq 1 100000 | (cat) | head -3
count_up() { seq 1 100000; }
count_up | cat | head -2

# A builtin that re-runs a command as a pipeline stage, checked against dash.
# The pipe ends must reach the real fd 0, 1, and 2 so the sub-command the
# builtin spawns inherits them.

# eval running an external command reads the pipe and writes the next stage.
printf 'axbxc\n' | eval sed 's/x/_/g'

# eval running cat consumes the producer rather than the terminal.
echo hi | eval cat
echo hi | eval 'cat'

# eval running a compound command reads every line of the pipe.
printf 'a\nb\n' | eval 'while read l; do echo "[$l]"; done'

# command running an external command reads the pipe too.
echo data | command cat

# eval feeding a counting stage sees the whole input.
seq 1 3 | eval wc -l

# A nested eval still threads the pipe to the innermost command.
echo nested | eval 'eval cat'

# eval as a middle stage wires the pipe through both sides.
seq 1 4 | eval cat | wc -l

# command runs the operand and its arguments from the pipe.
printf 'a b c\n' | command tr ' ' '\n' | wc -l

# A function shadows a builtin and a program inside a pipeline, like dash does.
myfunc() { echo from_function; }
myfunc | cat
cd() { echo cd_function; }
cd | cat
greet() { echo "hi $1"; }
echo ignored_input | greet world
