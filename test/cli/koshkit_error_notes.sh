# The koshkit usage errors carry a how-to-fix note under the located message.
# Each utility is reached through `koshkit <name>` so a binary on PATH does not
# shadow the bundled one.
unset KOSH_FLAGS

echo "=== killall arg count ==="
"$BIN" -c 'koshkit killall a b' 2>&1

echo "=== seq zero increment ==="
"$BIN" -c 'koshkit seq 1 0 10' 2>&1

echo "=== ln without -s ==="
"$BIN" -c 'koshkit ln a b' 2>&1

echo "=== tr one set ==="
"$BIN" -c 'koshkit tr abc' 2>&1

echo "=== find bad -type ==="
"$BIN" -c 'koshkit find . -type x' 2>&1
