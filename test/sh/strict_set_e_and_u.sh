#!/bin/sh
# set -e and set -u POSIX semantics, checked against dash. The errexit option
# does not trigger inside a condition, a negation, or the left of && and ||, and
# nounset stops the script at the first reference to an unset name.

set -e
echo start

# A failing command on the left of && or || does not exit under errexit.
false && echo unreached
echo after_and

# A failing command in an if condition does not exit.
if false; then echo no; fi
echo after_if

# A failing command before || that recovers does not exit.
false || echo recovered

# A negated failing command does not exit.
! false
echo after_negation

# A failing command in a while condition does not exit.
while false; do echo loop; done
echo after_while
set +e

# nounset prints nothing for the bad reference and the script stops there.
set -u
echo before_unset
# shellcheck disable=SC2154
echo "$never_assigned_name"
echo never_reached
