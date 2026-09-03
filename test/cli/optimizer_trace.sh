unset KOSH_FLAGS

echo "=== constant arithmetic fold ==="
"$BIN" --show-optimizer-diagnostics -c 'echo $((1 + 2 * 3))'

echo "=== exact constant arithmetic fold ==="
"$BIN" --show-optimizer-diagnostics -c 'echo $((2 ** 256))'

echo "=== constant propagation into arithmetic ==="
"$BIN" --show-optimizer-diagnostics -c 'x=2; y=3; echo $((x + y))'

echo "=== DEBUG observes folded arithmetic source ==="
"$BIN" --show-optimizer-diagnostics -c "x=2; trap 'x=9' DEBUG; echo \$((x + 1))"

echo "=== dead branch, condition is true ==="
"$BIN" --show-optimizer-diagnostics -c 'if true; then echo a; else echo b; fi'

echo "=== dead branch, all false folds to else ==="
"$BIN" --show-optimizer-diagnostics -c 'if false; then echo a; else echo b; fi'

echo "=== while false is eliminated ==="
"$BIN" --show-optimizer-diagnostics -c 'while false; do echo never; done; echo after'

echo "=== until true is eliminated ==="
"$BIN" --show-optimizer-diagnostics -c 'until true; do echo never; done; echo after'

echo "=== runtime variable does not fold ==="
"$BIN" --show-optimizer-diagnostics -c 'n="$(printf 0)"; echo $((n + 1))' 2>&1 | ./normalize-trace.sh "$BIN"

echo "=== undecidable condition does not fold ==="
"$BIN" --show-optimizer-diagnostics -c 'if [ -f /nonexistent_optimizer_probe ]; then echo a; fi; echo done'

unset KOSH_FLAGS

echo "=== if with no reachable body is eliminated ==="
"$BIN" --show-optimizer-diagnostics -c 'if false; then echo a; fi; echo done'

echo "=== for over an empty list is eliminated ==="
"$BIN" --show-optimizer-diagnostics -c 'for x in; do echo a; done; echo done'

echo "=== c-style for with a blank init and a zero condition is eliminated ==="
"$BIN" --show-optimizer-diagnostics -c 'for ((; 0; i++)); do echo a; done; echo done'

echo "=== c-style for with a non-blank init folds but keeps the init ==="
"$BIN" --show-optimizer-diagnostics -c 'for ((i=5; 0; i++)); do echo a; done; echo done'

echo "=== while false is eliminated ==="
"$BIN" --show-optimizer-diagnostics -c 'while false; do echo a; done; echo done'
