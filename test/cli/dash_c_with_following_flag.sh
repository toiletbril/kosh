unset KOSH_FLAGS
# A recognized flag that follows -c is parsed as a flag, and the -c command is
# taken from the first non-option operand the way bash reads it. A regression
# made -c swallow the following flag as its command and then run that flag as a
# program. The output is filtered to the markers so a sourced login profile
# cannot perturb it.
echo "== a bool flag after -c is parsed as a flag, the command still runs:"
"$BIN" -c -f 'echo marker-noglob' 2>/dev/null | grep -E '^marker-'
echo "== the login flag after -c does not become the -c command:"
"$BIN" -c -l 'echo marker-login' 2>/dev/null | grep -E '^marker-'
