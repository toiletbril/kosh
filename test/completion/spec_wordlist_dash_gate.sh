# A -W word list offers its dash entries only when the token already starts
# with a dash, the empty argument token completes the plain words, and a list
# reduced to nothing by the gate falls through to the filesystem.
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
touch "$dir/datafile"
cd "$dir"
echo "== empty token, mixed list:"
"$BIN" -c 'complete -W "alpha beta -x --why" mycmd' --debug-complete-at 'mycmd ' </dev/null
echo "== dash token, mixed list:"
"$BIN" -c 'complete -W "alpha beta -x --why" mycmd' --debug-complete-at 'mycmd -' </dev/null
echo "== empty token, flags-only list falls to files:"
"$BIN" -c 'complete -W "-x --why" flagcmd' --debug-complete-at 'flagcmd ' </dev/null
echo "== progcomp off ignores the registered specification:"
"$BIN" -c 'complete -W alpha mycmd; shopt -u progcomp' --debug-complete-at 'mycmd a' </dev/null
echo "== progcomp_alias off ignores the aliased specification:"
"$BIN" -c 'alias surface=target; complete -W alpha target' --debug-complete-at 'surface a' </dev/null
echo "== progcomp_alias on follows the aliased specification:"
"$BIN" -c 'alias surface=target; complete -W alpha target; shopt -s progcomp_alias' --debug-complete-at 'surface a' </dev/null
