DIRECTORY=$(mktemp -d) || exit 1
trap '[ -n "$DIRECTORY" ] && /bin/rm -rf "$DIRECTORY"' EXIT
touch "$DIRECTORY/a b" "$DIRECTORY/a*star"

. ../completions/kosh.bash

cd "$DIRECTORY" || exit 1
COMP_WORDS=(kosh a)
COMP_CWORD=1
_kosh_complete
printf '<%s>\n' "${COMPREPLY[@]}" | sort

COMP_WORDS=(kosh --debug)
COMP_CWORD=1
_kosh_complete
printf 'release-debug-flags=%s\n' "${#COMPREPLY[@]}"

COMP_WORDS=(kosh --as-language)
COMP_CWORD=1
_kosh_complete
printf 'as-language-server=<%s>\n' "${COMPREPLY[0]}"

COMP_WORDS=(kosh --enable-extended)
COMP_CWORD=1
_kosh_complete
printf 'extended-arithmetic=<%s>\n' "${COMPREPLY[0]}"

printf 'calc-flags=<%s>\n' "$(_koshkit_util_flags calc)"
printf 'head-flags=<%s>\n' "$(_koshkit_util_flags head)"
printf 'killall-flags=<%s>\n' "$(_koshkit_util_flags killall)"
printf 'ls-flags=<%s>\n' "$(_koshkit_util_flags ls)"
printf 'make-flags=<%s>\n' "$(_koshkit_util_flags make)"
printf 'mkdir-flags=<%s>\n' "$(_koshkit_util_flags mkdir)"
printf 'ps-flags=<%s>\n' "$(_koshkit_util_flags ps)"
printf 'rm-flags=<%s>\n' "$(_koshkit_util_flags rm)"
printf 'tail-flags=<%s>\n' "$(_koshkit_util_flags tail)"
printf 'which-flags=<%s>\n' "$(_koshkit_util_flags which)"

COMP_WORDS=(koshkit whoami --h)
COMP_CWORD=2
_koshkit_complete
printf 'whoami-help=<%s>\n' "${COMPREPLY[0]}"
