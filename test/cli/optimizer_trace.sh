unset SHIT_FLAGS
# The optimizer prepass runs in the default mood, so --show-optimizer-state traces
# what each pass folds. Each case is a separate process, so its stderr trace
# prints before its stdout result. The goldens assert the behavior of constant
# folding, arithmetic memoization through constant propagation, dead-branch
# elimination, and loop elimination, plus the cases that must not fold.

echo "=== constant arithmetic fold ==="
"$BIN" --show-optimizer-state -c 'echo $((1 + 2 * 3))'

echo "=== constant propagation into arithmetic ==="
"$BIN" --show-optimizer-state -c 'x=2; y=3; echo $((x + y))'

echo "=== dead branch, condition is true ==="
"$BIN" --show-optimizer-state -c 'if true; then echo a; else echo b; fi'

echo "=== dead branch, all false folds to else ==="
"$BIN" --show-optimizer-state -c 'if false; then echo a; else echo b; fi'

echo "=== while false is eliminated ==="
"$BIN" --show-optimizer-state -c 'while false; do echo never; done; echo after'

echo "=== until true is eliminated ==="
"$BIN" --show-optimizer-state -c 'until true; do echo never; done; echo after'

echo "=== runtime variable does not fold ==="
"$BIN" --show-optimizer-state -c 'n="$(printf 0)"; echo $((n + 1))' 2>&1 | ./normalize-trace.sh "$BIN"

echo "=== undecidable condition does not fold ==="
"$BIN" --show-optimizer-state -c 'if [ -f /nonexistent_optimizer_probe ]; then echo a; fi; echo done'

unset SHIT_FLAGS
# The --show-optimizer-state flag prints the optimizer state and a located line
# for every node the analysis stage eliminated, then a final summary, all to
# standard error. Each case is a separate process, so its stderr dump prints
# before its stdout result. The goldens assert the located elimination lines and
# the state summary across compound-body elimination, C-style for folding, and
# the empty for loop.

echo "=== if with no reachable body is eliminated ==="
"$BIN" --show-optimizer-state -c 'if false; then echo a; fi; echo done'

echo "=== for over an empty list is eliminated ==="
"$BIN" --show-optimizer-state -c 'for x in; do echo a; done; echo done'

echo "=== c-style for with a blank init and a zero condition is eliminated ==="
"$BIN" --show-optimizer-state -c 'for ((; 0; i++)); do echo a; done; echo done'

echo "=== c-style for with a non-blank init folds but keeps the init ==="
"$BIN" --show-optimizer-state -c 'for ((i=5; 0; i++)); do echo a; done; echo done'

echo "=== while false is eliminated ==="
"$BIN" --show-optimizer-state -c 'while false; do echo a; done; echo done'
