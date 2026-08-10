#!/bin/sh
# return, break, and continue crossing dot-source, eval, and loop boundaries,
# checked against dash. These are the regressions the source-buffer migration
# exposed, where a return must end a dot-source but propagate through eval.

# A return at the top of a dot-sourced file ends the source with its status, and
# execution continues after the dot command.
printf 'echo in_source\nreturn 3\necho never\n' > /tmp/kosh_compat_src_$$.sh
. /tmp/kosh_compat_src_$$.sh
echo "after_source=$?"

# Sourcing inside a function returns from the source, not the function, so the
# function keeps running after the dot command.
sources_a_file() {
  . /tmp/kosh_compat_src_$$.sh
  echo "still_in_function=$?"
}
sources_a_file
echo "after_function=$?"

# A continue inside a sourced file propagates into the enclosing loop, so the
# echo after the dot command is skipped on every iteration.
printf 'continue\n' > /tmp/kosh_compat_cont_$$.sh
for value in 1 2 3; do
  . /tmp/kosh_compat_cont_$$.sh
  echo "loop_skipped=$value"
done
echo "after_loop"

rm -f /tmp/kosh_compat_src_$$.sh /tmp/kosh_compat_cont_$$.sh

# A return inside eval propagates rather than ending the eval, so at the top of
# a non-interactive script it ends the shell with the given status. Nothing
# after this line runs in either shell.
eval "return 5"
echo "this line never runs"
