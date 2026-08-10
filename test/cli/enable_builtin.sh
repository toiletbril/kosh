unset KOSH_FLAGS
# enable is accepted as a no-op since every builtin is always enabled in kosh.
# -a lists every builtin, named args return 0 for known builtins and 1 for
# unknown, and the flags -n, -f, -s are accepted without effect.
echo "== enable with no args returns 0:"
"$BIN" -c 'enable'; echo "rc=$?"
echo "== enable compgen unset returns 0:"
"$BIN" -c 'enable compgen unset'; echo "rc=$?"
echo "== enable -n echo returns 0:"
"$BIN" -c 'enable -n echo'; echo "rc=$?"
echo "== enable -a lists builtins:"
"$BIN" -c 'enable -a' | head -3
echo "== enable -f file echo returns 0:"
"$BIN" -c 'enable -f /tmp/nope echo'; echo "rc=$?"
echo "== enable -s returns 0:"
"$BIN" -c 'enable -s'; echo "rc=$?"
echo "== enable nosuchbuiltin reports and exits 1:"
"$BIN" -c 'enable nosuchbuiltin' 2>&1; echo "rc=$?"
echo "== builtin enable compgen unset forwards through enable:"
"$BIN" -c 'builtin enable compgen unset; echo ok'
