echo "== aliased subcommands:"
"$BIN" -c 'complete -W "check-ref-format --version" target; alias g=target; shopt -s progcomp_alias' \
    --debug-complete-at 'g check-ref' </dev/null
echo "== aliased options:"
"$BIN" -c 'complete -W "check-ref-format --version" target; alias g=target; shopt -s progcomp_alias' \
    --debug-complete-at 'g --vers' </dev/null
