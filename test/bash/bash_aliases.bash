#!/bin/bash
# Bash BASH_ALIASES behavior, checked byte-for-byte against bash. This covers
# live reads, element and scalar writes, element and whole-array unsets, alias
# removal, alias preservation after whole-array unset, and an ordinary
# associative array created by redeclaring the disconnected name.
alias first='echo one'
alias second='echo two'
printf 'first=%s second=%s count=%s\n' \
  "${BASH_ALIASES[first]}" "${BASH_ALIASES[second]}" \
  "${#BASH_ALIASES[@]}"

BASH_ALIASES[third]='echo three'
alias third >/dev/null
printf 'third=%s alias-status=%s\n' "${BASH_ALIASES[third]}" "$?"
BASH_ALIASES[first]='echo changed'
printf 'changed=%s\n' "${BASH_ALIASES[first]}"

unset 'BASH_ALIASES[first]'
printf 'element-unset=%s\n' "${BASH_ALIASES[first]}"
BASH_ALIASES=()
printf 'empty-assignment-count=%s\n' "${#BASH_ALIASES[@]}"
unalias second
printf 'unalias=%s count=%s\n' "${BASH_ALIASES[second]-missing}" \
  "${#BASH_ALIASES[@]}"
BASH_ALIASES=zero
printf 'scalar=%s\n' "${BASH_ALIASES[0]}"

unset BASH_ALIASES
printf 'whole-unset=%s\n' "${BASH_ALIASES[third]-missing}"
alias third >/dev/null
printf 'alias-survives=%s\n' "$?"
declare -A BASH_ALIASES
BASH_ALIASES[ordinary]='echo ordinary'
alias ordinary >/dev/null 2>&1
printf 'ordinary-alias-status=%s\n' "$?"
alias live='echo live'
printf 'ordinary-live=%s ordinary-value=%s\n' \
  "${BASH_ALIASES[live]-missing}" "${BASH_ALIASES[ordinary]}"
