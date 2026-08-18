set -e

result=$("$BIN" --debug-highlight-at \
    'V=1; unset V; unset NOPE; export SHIPPED; declare -a rows; readonly frozen=2; local scoped; typeset -i counted; unset -v V; echo $SHIPPED $rows')
tab=$(printf '\t')
printf '%s\n' "$result" |
    grep -E "${tab}(unset-variable|variable|assignment-name|flag)$"
