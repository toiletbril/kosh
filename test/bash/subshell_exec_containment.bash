#!/bin/bash
# A bare exec inside an in-process subshell or a command substitution must not
# move the parent's descriptors, the containment a forked subshell gets for
# free, while a top-level exec still persists.
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
( exec </dev/null )
read -r line <<EOF
survives
EOF
echo "stdin=$line"
( exec >/dev/null; echo swallowed )
echo "stdout=visible"
x=$( exec >/dev/null; echo inner )
echo "subst=[$x]"
exec 9>&-
exec 3>/dev/null 4>/dev/null 5>/dev/null 6>/dev/null 7>/dev/null 8>/dev/null
( exec 9>"$dir/nine" )
{ echo leak >&9; } 2>/dev/null || echo "fd9=contained"
exec 3>&- 4>&- 5>&- 6>&- 7>&- 8>&-
( exec 2>"$dir/two"; echo contained-err >&2 )
echo "after-stderr=ok" >&2 2>/dev/null
echo "stderr-file=$(cat "$dir/two")"
exec 6>"$dir/six"
echo persisted >&6
exec 6>&-
echo "toplevel=$(cat "$dir/six")"


# A readonly or a declare -i made inside a subshell dies with the child, so the
# parent can still reassign the name afterward, the way a forked subshell
# isolates its option and attribute changes.
x=1
(readonly x)
x=2
echo "x=$x"
n=5
(declare -i n)
n=abc
echo "n=$n"
arr=(a b)
(readonly arr)
arr=(c d)
echo "arr=${arr[1]}"

# A fatal expansion error inside a command substitution, checked against bash.
# The error exits only the substitution subshell, so the parent assignment gets
# the empty result and the script continues rather than aborting.
echo start
result=$(echo ${undef_var:?the message})
echo "after: [$result]"
echo end

# A not-found command in a pipeline stage does not abort the rest, so a later
# stage still runs and the pipeline status is the last stage's, matching bash.
nonexistent_cmd_xyz_123 | cat
echo "after=$?"
echo start | nonexistent_cmd_xyz_123 | wc -l | tr -d ' '
