#!/bin/bash
# Bash DIRSTACK behavior, checked byte-for-byte against bash. This covers the
# current directory at index zero, reverse saved-stack order, scalar and whole
# assignments, negative and out-of-range indices, inert element unsets, whole
# unset preservation, and an ordinary array redeclared after disconnection.
cd /
pushd /tmp >/dev/null
pushd /usr >/dev/null
printf 'initial=<%s> count=%s scalar=%s negative=%s\n' \
  "${DIRSTACK[*]}" "${#DIRSTACK[@]}" "$DIRSTACK" "${DIRSTACK[-1]}"

local_plain_dirstack() {
  local DIRSTACK
  printf 'local-plain=<%s> set=<%s> dirs=<%s>\n' \
    "$DIRSTACK" "${DIRSTACK+x}" "$(dirs)"
}
local_array_dirstack() {
  local -a DIRSTACK=(local array)
  printf 'local-array=<%s> set=<%s> dirs=<%s>\n' \
    "${DIRSTACK[*]}" "${DIRSTACK+x}" "$(dirs)"
}
local_inherited_dirstack() {
  local DIRSTACK
  printf 'local-inherited=<%s> dirs=<%s>\n' "${DIRSTACK[*]}" "$(dirs)"
}
local_plain_dirstack
local_array_dirstack
shopt -s localvar_inherit
local_inherited_dirstack
shopt -u localvar_inherit
printf 'local-restored=<%s>\n' "${DIRSTACK[*]}"

DIRSTACK[0]=ignored
DIRSTACK[1]=one
DIRSTACK[8]=eight
printf 'elements=<%s> dirs=<%s>\n' "${DIRSTACK[*]}" "$(dirs -p)"
unset 'DIRSTACK[1]'
printf 'element-unset=<%s>\n' "${DIRSTACK[*]}"

DIRSTACK=(ignored two three four)
printf 'whole=<%s> dirs=<%s>\n' "${DIRSTACK[*]}" "$(dirs -p)"
DIRSTACK[-1]=last
DIRSTACK+=(tail)
printf 'negative-and-append=<%s>\n' "${DIRSTACK[*]}"

unset DIRSTACK
printf 'unset=<${DIRSTACK+x}> dirs=<%s>\n' "$(dirs -p)"
declare -a DIRSTACK
DIRSTACK=(ordinary array)
pushd /bin >/dev/null
printf 'ordinary=<%s> dirs=<%s>\n' "${DIRSTACK[*]}" "$(dirs -p)"
