echo "== stdin reads one top-level command:"
printf 'echo one\necho two\n' | "$BIN" -t

echo "== a command string runs as one input command:"
"$BIN" -t -c 'echo one; echo two'

echo "== a script can enable onecmd while reading:"
printf 'set -o onecmd\necho after\n' | "$BIN"

echo "== the option is queryable:"
"$BIN" -c "set -o onecmd; set -o | koshkit grep '^onecmd'"
