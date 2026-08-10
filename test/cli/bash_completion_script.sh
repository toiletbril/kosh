DIRECTORY=$(mktemp -d) || exit 1
trap '[ -n "$DIRECTORY" ] && /bin/rm -rf "$DIRECTORY"' EXIT
touch "$DIRECTORY/a b" "$DIRECTORY/a*star"

. ../completions/shit.bash

cd "$DIRECTORY" || exit 1
COMP_WORDS=(shit a)
COMP_CWORD=1
_shit_complete
printf '<%s>\n' "${COMPREPLY[@]}" | sort

COMP_WORDS=(shit --debug)
COMP_CWORD=1
_shit_complete
printf 'release-debug-flags=%s\n' "${#COMPREPLY[@]}"
