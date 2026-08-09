# A spec that replies nothing never claims the completion, so the operand
# falls to the filesystem, the cp case whose loaded function offers only
# options. The dash token still reads the spec's options. The probe runs in
# a temp directory holding one known file, so the filesystem fallback has a
# deterministic candidate.
dir=$(mktemp -d)
touch "$dir/fallback_probe.txt"
echo "== empty COMPREPLY falls to files:"
"$BIN" -c "cd '$dir' || exit; _f(){ COMPREPLY=(); }; complete -F _f cp" --debug-complete-at 'cp fallb' </dev/null
echo "== retry status 124 falls to files:"
"$BIN" -c "cd '$dir' || exit; _f(){ return 124; }; complete -F _f cp" --debug-complete-at 'cp fallb' </dev/null
echo "== dash-only reply on an operand falls to files:"
"$BIN" -c "cd '$dir' || exit; _f(){ COMPREPLY=(--archive); }; complete -F _f cp" --debug-complete-at 'cp fallb' </dev/null
echo "== the dash token keeps the spec options:"
"$BIN" -c "cd '$dir' || exit; _f(){ COMPREPLY=(--archive --backup); }; complete -F _f cp" --debug-complete-at 'cp --ar' </dev/null
rm -rf "$dir"
