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
