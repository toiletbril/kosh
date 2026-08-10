# A cobra-style completion function appends "  (description)" to each value for
# its own menu. kosh renders its own dimmed description column, so the value is
# split from the description and only the bare value lands in the candidate
# list. The driver prints candidates alone, so a stripped value here proves the
# split ran. A value that carries no description passes through unchanged, and a
# value holding a parenthesis with no leading space is left whole.
echo "== cobra-style values split off the description:"
"$BIN" -c "_f(){ COMPREPLY=('void.ts.net  (the void node)' 'dupa.ts.net  (dupa node)'); }; complete -F _f ts" --debug-complete-at 'ts ' </dev/null
echo "== a value with no description is unchanged:"
"$BIN" -c "_f(){ COMPREPLY=('plainvalue'); }; complete -F _f tc" --debug-complete-at 'tc ' </dev/null
echo "== a parenthesis inside a value is left whole:"
"$BIN" -c "_f(){ COMPREPLY=('file(1).txt'); }; complete -F _f tp" --debug-complete-at 'tp ' </dev/null
