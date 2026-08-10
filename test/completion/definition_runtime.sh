unset SHIT_FLAGS

directory=
trap 'test -n "$directory" && /bin/rm -rf "$directory"' EXIT
directory=$(mktemp -d)
touch "$directory/apple.c" "$directory/apple.txt"

echo "== callback keeps definition diagnostics:"
completion_output=$("$BIN" -c "cd '$directory' || exit; set -WW; _helper(){ eval 'printf \"%s\\n\" no_match_*'; }; _f(){ _helper >/dev/null; COMPREPLY=(ok); }; complete -F _f vim; set --mood bash" --debug-complete-at 'vim /etc/' </dev/null 2>"$directory/warnings")
if grep -q "glob pattern 'no_match_" "$directory/warnings"; then
    echo warning-leaked
else
    echo warning-clean
fi
printf '%s\n' "$completion_output"

echo "== callback keeps definition annoying state:"
"$BIN" -c 'set +o annoying-diagnostics; _f(){ if [[ -o annoying-diagnostics ]]; then COMPREPLY=(on); else COMPREPLY=(off); fi; }; complete -F _f annoyingcmd; set -o annoying-diagnostics' --debug-complete-at 'annoyingcmd ' </dev/null

echo "== callback keeps definition mood:"
"$BIN" -c '_f(){ COMPREPLY=("$(set --mood)"); }; complete -F _f moodcmd; set --mood bash' --debug-complete-at 'moodcmd ' </dev/null

echo "== word list keeps registration mood:"
"$BIN" -M bash-posix -c "complete -W '\$(set --mood)' moodcmd; set --mood shit" --debug-complete-at 'moodcmd ' </dev/null

echo "== callback can replace its spec:"
"$BIN" -M bash -c '_f(){ complete -W replacement selfcmd; COMPREPLY=(stable); }; complete -F _f selfcmd' --debug-complete-at 'selfcmd ' </dev/null

echo "== compgen exclusion filter:"
(
    cd "$directory" || exit 1
    "$BIN" -M bash -c "compgen -f -X '*.txt' -- app; compgen -f -X '!*.txt' -- app; compgen -f -X '&*.txt' -- app"
    "$BIN" -M bash -c "compgen -W 'a* ax' -X 'a\\*' -- a; compgen -W 'a&x aax' -X '*\\&*' -- a; compgen -W '!a ba' -X '\\!*' -- ''"
    "$BIN" -M bash -c "compgen -W 'a* a*foo' -X '&' -- 'a*'"
)

test -n "$directory" && /bin/rm -rf "$directory"
directory=
