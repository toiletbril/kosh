# A bare unset operand completes shell and environment variable names with no
# leading dollar, while unset -f completes the defined function names instead, so
# the operand matches the kind of name the flag removes.
echo "== unset completes a variable name:"
"$BIN" -c 'completion_probe_var=1' --debug-complete-at 'unset completion_probe' </dev/null

echo "== unset completes a dynamic ANSI variable name:"
"$BIN" --debug-complete-at 'unset KOSH_ANSI_RE' </dev/null

echo "== unset -f completes a function name:"
"$BIN" -c 'completion_probe_fn() { :; }' --debug-complete-at 'unset -f completion_probe' </dev/null

echo "== plain unset does not offer a function name:"
"$BIN" -c 'completion_probe_fn() { :; }' --debug-complete-at 'unset completion_probe' </dev/null
