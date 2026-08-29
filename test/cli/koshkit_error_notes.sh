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

echo "=== readlink extra operand location ==="
"$BIN" -c 'koshkit readlink first second' 2>&1

echo "=== fuser conflicting flags location ==="
"$BIN" -c 'koshkit fuser -cf file' 2>&1
echo "=== whoami operand location ==="
"$BIN" -c 'koshkit whoami extra' 2>&1

echo "=== nice increment location ==="
"$BIN" -c 'koshkit nice -n nope true' 2>&1
echo "=== nice command location ==="
"$BIN" -c 'koshkit nice KOSH_MISSING_COMMAND' 2>&1

echo "=== grep pattern location ==="
"$BIN" -c "koshkit grep '['" 2>&1
echo "=== grep file location ==="
"$BIN" -c 'koshkit grep value KOSH_MISSING_FILE' 2>&1

echo "=== mv source location ==="
"$BIN" -c 'koshkit mv KOSH_MISSING_SOURCE KOSH_MISSING_DESTINATION' 2>&1

echo "=== renice identifier location ==="
"$BIN" -c 'koshkit renice -n 1 nope' 2>&1

echo "=== date operand location ==="
"$BIN" -c 'koshkit date 2026' 2>&1

echo "=== getconf variable location ==="
"$BIN" -c 'koshkit getconf KOSH_MISSING_CONFIGURATION' 2>&1

echo "=== locale name location ==="
"$BIN" -c 'koshkit locale KOSH_MISSING_LOCALE' 2>&1

echo "=== id user location ==="
"$BIN" -c 'koshkit id KOSH_MISSING_USER' 2>&1

echo "=== stty setting location ==="
"$BIN" -c 'koshkit stty KOSH_MISSING_SETTING' 2>&1

echo "=== stty conflicting flag location ==="
"$BIN" -c 'koshkit stty -a -g' 2>&1

echo "=== logger priority location ==="
"$BIN" -c 'koshkit logger -p KOSH_MISSING_PRIORITY message' 2>&1

echo "=== nohup command location ==="
"$BIN" -c 'koshkit nohup KOSH_MISSING_COMMAND' 2>&1
