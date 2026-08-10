unset KOSH_FLAGS
# A malformed [[ =~ ]] regex points its caret at the regex operand and names the
# offending pattern, rather than pointing the bare [[.
"$BIN" -c '[[ abc =~ ( ]]' 2>&1
