#!/bin/sh
# The test builtin and arithmetic expansion, checked against dash.

test 1 -eq 1 && echo "eq_ok"
test 2 -ne 1 && echo "ne_ok"
[ abc = abc ] && echo "streq_ok"
[ -z "" ] && echo "empty_ok"
[ -n "x" ] && echo "nonempty_ok"
[ 5 -gt 3 ] && echo "gt_ok"
[ 3 -lt 5 ] && echo "lt_ok"

echo "sum=$((2 + 3 * 4))"
x=10
echo "mul=$((x * 2))"
echo "ref=$(($x + 5))"
d=0
while [ "$d" -le 3 ]; do
    echo "count=$d"
    d=$((d + 1))
done

case hello in
    h*) echo "case_glob" ;;
    *) echo "case_default" ;;
esac


# The test builtin string and integer operators, checked against dash. The
# string predicates judge length and equality, the integer predicates compare
# numerically, and the negation and the and-or combine results.

# String length predicates.
[ -z "" ] && echo "empty_is_zero"
[ -n "x" ] && echo "nonempty_is_n"

# String equality and inequality.
[ abc = abc ] && echo "streq"
[ abc != abd ] && echo "strneq"

# Integer comparisons across the full set of operators.
[ 5 -eq 5 ] && echo "eq"
[ 5 -ne 6 ] && echo "ne"
[ 4 -lt 5 ] && echo "lt"
[ 5 -le 5 ] && echo "le"
[ 6 -gt 5 ] && echo "gt"
[ 5 -ge 5 ] && echo "ge"

# A negation flips the result.
[ ! -z "x" ] && echo "negated_zero"

# A combined expression evaluates both halves.
[ 1 -lt 2 ] && [ 2 -lt 3 ] && echo "chained_and"

# The single-bracket form reports the comparison status through the exit code.
if [ "abc" = "xyz" ]; then
    echo "wrong"
else
    echo "else_branch"
fi

# A numeric string compares as a number, not as text.
if [ 010 -eq 10 ]; then
    echo "numeric_equal"
fi

# The test builtin's file comparison operators -ef, -nt, and -ot, checked
# against dash. The timestamps are set explicitly so the ordering is stable.

base=/tmp/kosh_filecmp_$$
older="$base.old"
newer="$base.new"
rm -f "$older" "$newer" "$base.link"
: > "$older"
: > "$newer"
touch -t 202001010000 "$older"
touch -t 203001010000 "$newer"

test "$newer" -nt "$older" && echo "newer_nt_older"
test "$older" -ot "$newer" && echo "older_ot_newer"
test "$older" -nt "$newer" || echo "older_not_nt_newer"

# A file is the same file as itself, and a hard link names the same file.
test "$older" -ef "$older" && echo "self_ef"
ln "$older" "$base.link"
test "$older" -ef "$base.link" && echo "hardlink_ef"
test "$older" -ef "$newer" || echo "distinct_not_ef"

# A comparison against a missing file is false.
rm -f "$base.gone"
test "$older" -ef "$base.gone" || echo "missing_not_ef"

rm -f "$older" "$newer" "$base.link"

# -L and -h test the symlink-ness of a path without following it.
target=/tmp/kosh_symlink_target_fixed_$$
link=/tmp/kosh_symlink_link_fixed_$$
rm -f "$link"
: > "$target"
ln -s "$target" "$link"
[ -L "$link" ] && echo link_is_symlink
[ -h "$link" ] && echo link_h_symlink
[ ! -L "$target" ] && echo target_not_symlink
[ -L "$target" ] || echo target_confirmed_plain
[ ! -L "$link" ] || echo negation_of_symlink_false
rm -f "$link" "$target"
# Other POSIX file-type primaries, deterministic against a redirected stdout.
[ -c /dev/null ] && echo devnull_is_char
[ -b /dev/null ] || echo devnull_not_block
[ -t 1 ] && echo stdout_is_tty || echo stdout_not_tty
[ -p /etc ] || echo etc_not_fifo
[ -S /etc ] || echo etc_not_socket
