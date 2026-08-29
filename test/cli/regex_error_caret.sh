unset KOSH_FLAGS
# A malformed [[ =~ ]] regex points its caret at the regex operand and names the
# offending pattern, rather than pointing the bare [[.
"$BIN" -c '[[ abc =~ ( ]]' 2>&1
"$BIN" -c '[[ aa =~ (a|aa) ]]; printf "longest=%s\n" "${BASH_REMATCH[1]}"'
"$BIN" -c '[[ b =~ (a)?b ]]; printf "optional=<%s>\n" "${BASH_REMATCH[1]}"'
"$BIN" -c 'shopt -s nocasematch; [[ AbC =~ ^abc$ ]]; echo "nocase=$?"'
