# Completion re-roots to the command line of the substitution the cursor sits
# in, so a token inside $(...), inside backticks, or inside the substitution of
# a compound head completes the inner command rather than the outer one. A
# registered spec and a controlled directory keep the candidates stable across
# machines.
dir=$(mktemp -d)
trap '[ -n "$dir" ] && /bin/rm -rf "$dir"' EXIT

echo "== inside \$( ):"
"$BIN" -c 'complete -W "alpha beta gamma" probecmd' --debug-complete-at 'echo $(probecmd a' </dev/null

echo "== before a closed substitution delimiter:"
line='echo $(probecmd a)'
KOSH_TEST_COMPLETE_CURSOR=$((${#line} - 1)) \
    "$BIN" -c 'complete -W "alpha beta gamma" probecmd' \
    --debug-complete-at "$line" </dev/null

echo "== command name before a closed substitution delimiter:"
printf '#!/bin/sh\n' > "$dir/innerprobe-command"
chmod +x "$dir/innerprobe-command"
line='echo $(innerprobe-c)'
KOSH_TEST_COMPLETE_CURSOR=$((${#line} - 1)) \
    env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$dir" \
    "$BIN" --debug-complete-at "$line" </dev/null

echo "== quoted close and outer suffix stay outside the active body:"
prefix='echo $(printf ")"; probecmd b'
line="$prefix) tail"
KOSH_TEST_COMPLETE_CURSOR=${#prefix} \
    "$BIN" -c 'complete -W "alpha beta gamma" probecmd' \
    --debug-complete-at "$line" </dev/null

echo "== innermost closed substitution owns the cursor:"
prefix='echo $(echo $(probecmd g'
line="$prefix))"
KOSH_TEST_COMPLETE_CURSOR=${#prefix} \
    "$BIN" -c 'complete -W "alpha beta gamma" probecmd' \
    --debug-complete-at "$line" </dev/null
echo "== a heredoc close does not close the substitution:"
prefix='echo $(cat <<EOF
)
EOF
probecmd a'
line="$prefix) tail"
KOSH_TEST_COMPLETE_CURSOR=${#prefix} \
    "$BIN" -c 'complete -W "alpha beta gamma" probecmd' \
    --debug-complete-at "$line" </dev/null
echo "== inside backticks:"
"$BIN" -c 'complete -W "alpha beta gamma" probecmd' --debug-complete-at 'echo `probecmd b' </dev/null
echo "== inside a for-head substitution:"
"$BIN" -c 'complete -W "alpha beta gamma" probecmd' --debug-complete-at 'for x in $(probecmd g' </dev/null
echo "== filesystem still completes inside a substitution:"
: > "$dir/onlyfile"
filesystem_result=$("$BIN" \
    --debug-complete-at "echo \$(cat $dir/only" </dev/null)
test "$filesystem_result" = "$dir/onlyfile"
echo onlyfile
echo "== arithmetic is not a command body:"
(
    cd "$dir" &&
        "$BIN" -c 'complete -W "alpha beta gamma" probecmd' \
            --debug-complete-at 'echo $((probecmd a' </dev/null
)

echo "== a case pattern does not close the substitution:"
prefix='echo $(case x in x) probecmd a'
line="$prefix;; esac)"
KOSH_TEST_COMPLETE_CURSOR=${#prefix} \
    "$BIN" -c 'complete -W "alpha beta gamma" probecmd' \
    --debug-complete-at "$line" </dev/null

echo "== an operator-adjacent comment ends at its newline:"
prefix='echo $(true;# )
probecmd a'
line="$prefix)"
KOSH_TEST_COMPLETE_CURSOR=${#prefix} \
    "$BIN" -c 'complete -W "alpha beta gamma" probecmd' \
    --debug-complete-at "$line" </dev/null

echo "== a short prefix does not scan a large suffix:"
suffix=
for ((suffix_index = 0; suffix_index < 2048; suffix_index++)); do
  suffix+=' ignored'
done
prefix='echo $(probecmd a'
result=$(KOSH_TEST_COMPLETION_STATS=1 \
    KOSH_TEST_COMPLETE_CURSOR=${#prefix} \
    "$BIN" -c 'complete -W "alpha beta gamma" probecmd' \
    --debug-complete-at "$prefix$suffix" </dev/null)
candidate=${result%%$'\n'*}
statistics=${result##*$'\n'}
scanned_byte_count=${statistics#lexical-scan-bytes=}
case $scanned_byte_count in
'' | *[!0-9]*) is_scanned_byte_count_valid=false ;;
*) is_scanned_byte_count_valid=true ;;
esac
if [[ $candidate == alpha && $statistics == lexical-scan-bytes=* &&
      $is_scanned_byte_count_valid == true &&
      $scanned_byte_count -le 64 ]]; then
  echo bounded
else
  echo unbounded
fi
