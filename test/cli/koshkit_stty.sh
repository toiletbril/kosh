unset KOSH_FLAGS

BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d)
d=$(cd "$d" && pwd -P)
trap 'test -n "$d" && /bin/rm -r "$d"' EXIT

command_text="exec \"$BIN\" -c 'before=\$(koshkit stty -g) || exit
echo
koshkit stty KOSH_MISSING_SETTING 2>&1
overflow=10000000000000000:\${before#*:}
if koshkit stty \"\$overflow\" 2>/dev/null; then echo invalid=failed; else echo invalid=passed; fi
koshkit stty -echo igncr -opost tostop erase \"^H\" || exit
settings=\$(koshkit stty -a) || exit
case \"\$settings\" in *-echo*igncr*-opost*tostop*) echo modes=passed;; *) echo modes=failed;; esac
changed=\$(koshkit stty -g) || exit
if [ \"\$before\" != \"\$changed\" ]; then echo changed=passed; else echo changed=failed; fi
koshkit stty \"\$before\" || exit
after=\$(koshkit stty -g) || exit
if [ \"\$before\" = \"\$after\" ]; then echo restore=passed; else echo restore=failed; fi'"

output_pattern='^(3:14: error: stty: invalid terminal setting\.|     3 \|  koshkit stty KOSH_MISSING_SETTING|       \|               \^~~~~~~~~~~~~~~~~~~~|(invalid|modes|changed|restore)=)'
if script -q -c true /dev/null >/dev/null 2>&1; then
  script -q -c "$command_text" /dev/null | tr -d '\r' |
    grep -E "$output_pattern"
elif script -q /dev/null /usr/bin/true >/dev/null 2>&1; then
  script -q /dev/null /bin/sh -c "$command_text" | tr -d '\r' |
    grep -E "$output_pattern"
else
  exit 1
fi
