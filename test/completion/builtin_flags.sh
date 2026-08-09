# The builtin flag stage reads the registered FLAG lists, the set option
# table, the shopt names, kill's signal names, and the shell binary's own
# flags, all without a manpage.
echo "== set switches:"
"$BIN" --debug-complete-at 'set -' </dev/null
echo "== set -o names by prefix:"
"$BIN" --debug-complete-at 'set -o no' </dev/null
echo "== shit binary flags:"
"$BIN" --debug-complete-at 'shit --b' </dev/null
echo "== shit no-traces flag:"
"$BIN" --debug-complete-at 'shit --no-t' </dev/null
echo "== declare letters:"
"$BIN" --debug-complete-at 'declare -' </dev/null
echo "== kill signal names:"
signal_names=$("$BIN" --debug-complete-at 'kill -' </dev/null)
printf '%s\n' "$signal_names" | grep -E '^-(HUP|INT|KILL|QUIT|TERM)$'
if [ "${OS-}" != Windows_NT ]; then
    for signal_name in ABRT ALRM CONT PIPE STOP TSTP USR1 USR2; do
        printf '%s\n' "$signal_names" | grep -q "^-$signal_name$"
    done
fi
echo "== shopt names by prefix:"
"$BIN" --debug-complete-at 'shopt glob' </dev/null
echo "== read flags:"
"$BIN" --debug-complete-at 'read -' </dev/null
echo "== fc flags:"
"$BIN" --debug-complete-at 'fc -' </dev/null
