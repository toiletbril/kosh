#!/bin/bash
# Bash type -a for cd and type -t for a builtin, keywords, a function, and a name
# with no command, checked byte-for-byte against bash. PATH is pinned to a single
# directory so the type -a cd listing is deterministic. A usrmerge system carries
# /usr/sbin as a symlink to /usr/bin, so a PATH with both makes bash list the cd
# binary twice while kosh lists it once, a divergence the single entry removes.
PATH=/usr/bin
type -a cd
type -t cd
type -t if
type -t while
type -t for
type -t case
greet() { :; }
type -t greet
type -t no_such_command_zzz
echo "rc=$?"


# command -p resolves against a default PATH that finds the standard utilities,
# so it runs echo even when the caller's PATH is empty.
echo "normal=$(command -p echo hi)"
echo "empty_path=$(PATH= command -p echo hi)"

# Bash command -v for a builtin and a keyword, checked byte-for-byte against
# bash. A builtin and a keyword name print themselves, and a missing name yields
# nothing with status one.
command -v cd
command -v echo
command -v if
command -v while
command -v case
command -v no_such_command_zzz
echo "rc=$?"

# Bash builtin keyword forcing the builtin past a same-named function, checked
# byte-for-byte against bash.
echo() { command echo "wrapped: $*"; }
builtin echo direct call
echo via function
unset -f echo
builtin true; echo "true rc=$?"
builtin false; echo "false rc=$?"

printf 'dbracket=%s\n' "$(type -t '[[')"
printf 'dbracketclose=%s\n' "$(type -t ']]')"
printf 'bang=%s\n' "$(type -t '!')"
printf 'brace=%s\n' "$(type -t '{')"
printf 'function=%s\n' "$(type -t 'function')"
printf 'time=%s\n' "$(type -t 'time')"
printf 'if=%s\n' "$(type -t 'if')"
printf 'echo=%s\n' "$(type -t 'echo')"

type -t echo
type -t if
type -t while
f() { :; }
type -t f
type -t totally_nonexistent_xyz_cmd
echo "notfound rc=$?"
