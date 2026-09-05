unset KOSH_FLAGS
# A name that is not a builtin is a located error pointing at the operand.
echo "== help nope is a located error:"
"$BIN" -c 'help nope' 2>&1; echo "rc=$?"

echo "== time routes help to its builtin:"
"$BIN" -c 'time --help >/dev/null' 2>/dev/null
echo "rc=$?"

echo "== help formatting modes:"
"$BIN" -c 'help -d cd; help -s history; help -m cd' | head -n 10
"$BIN" -c 'help -dm cd; help -ms cd' | head -n 2
