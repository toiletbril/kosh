#!/bin/sh
# A subshell and a command substitution isolate environment changes from the
# parent the way a forked shell does, checked against dash. An export, an
# unset, or a reassignment of an exported name inside must not leak out, and a
# PATH change inside must not alter how the parent resolves a command.

export EXPORTED=parent
( EXPORTED=child; echo "inside_reassign=$EXPORTED" )
echo "after_reassign=$EXPORTED"

result=$( EXPORTED=substituted; echo captured )
echo "after_cmdsub=$EXPORTED result=$result"

( export EXPORTED=exported_in_subshell )
echo "after_subshell_export=$EXPORTED"

export REMOVED=present
( unset REMOVED; echo "inside_unset=[$REMOVED]" )
echo "after_unset=[$REMOVED]"

# A name exported only inside a subshell does not survive it.
( export FRESH=temporary )
echo "fresh=[$FRESH]"

# PATH changed inside a subshell does not change how the parent resolves a
# command, so an external utility still runs afterwards.
( PATH=/nonexistent ) 2>/dev/null
ls /dev/null >/dev/null 2>&1 && echo "external_still_resolves"


# A set -u read of an unset name aborts the subshell with status 2 the way
# dash reports it, and the parent goes on.
# shellcheck disable=SC2154
(set -u; echo "$not_set_here"; echo not_reached) 2>/dev/null
echo "status=$?"
echo survived
