#!/bin/bash
# Bash getopts in silent mode, checked byte-for-byte against bash. The leading
# colon selects silent error handling, so a bad option and a missing argument
# arrive through the question-mark and colon cases with OPTARG set, while OPTIND
# advances across grouped and separate options.
parse() {
  local opt OPTIND=1
  while getopts ":ab:c" opt; do
    case $opt in
      a) echo "flag a";;
      b) echo "b arg=$OPTARG";;
      c) echo "flag c";;
      \?) echo "bad opt: $OPTARG";;
      :) echo "missing arg: $OPTARG";;
    esac
  done
  echo "OPTIND=$OPTIND"
}
parse -a -b val -c
parse -ab val
parse -x
parse -b
parse -a foo


# kill -s names the signal by name and -n by number, apart from the -SIGNAL
# shorthand, while signal 0 only checks the target exists. A trapped self-signal
# proves the name resolves without any process dying.
trap 'echo caught-usr1' USR1
kill -s USR1 $$
echo "exists_s=$(kill -s 0 $$; echo $?)"
echo "exists_n=$(kill -n 0 $$; echo $?)"

# Bash let return status, checked byte-for-byte against bash. A let whose last
# expression is zero returns status one, a nonzero result returns status zero,
# and a relational expression returns the negation of its truth value.
let 'a = 1'; echo "rc=$?"
let 'b = 0'; echo "rc=$?"
let 'c = 5 - 5'; echo "rc=$?"
let 'd = 3 > 2'; echo "rc=$?"
let 'e = 2 > 3'; echo "rc=$?"
( let 'x = 0' ); echo "subshell rc=$?"
let; echo "empty rc=$?"

# return outside a function and outside a sourced file is rejected and the shell
# continues, while return inside a function ends it with the status. The dot
# command passes its trailing operands to the sourced file as positional
# parameters and restores the caller's parameters afterward.
return 5
echo "after=$?"
f() { return 7; }
f
echo "fn=$?"
file=$(mktemp)
printf 'echo "in=$1 $2"\n' > "$file"
set -- keep1 keep2
. "$file" arg1 arg2
echo "out=$1 $2"
rm -f "$file"

# trap -p prints the trap for a named condition with the SIG prefix bash uses, so
# the line reloads. The conditions are named so bash's own default job-control
# traps stay out of the comparison.
trap 'echo caught' INT
echo "p=$(trap -p INT)"
echo "bare=$(trap -p TERM)"

fc_file=$(mktemp)
printf 'echo alpha\necho old-old\nfalse\n' > "$fc_file"
history -c
history -r "$fc_file"
fc -ln -3 -1
fc -lrn -3 -1
fc -s old=new echo
echo "fc-substitute=$?"
fc -ln
history -c
history -r "$fc_file"
fc -ln 3 3
fc -e - false
echo "fc-status=$?"
rm -f "$fc_file"
