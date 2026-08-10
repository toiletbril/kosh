unset KOSH_FLAGS
# The funsub is a bash addition, so POSIX mode keeps the old reading where
# ${ is a parameter expansion and a space-led name is a syntax error rather
# than a command body. The default mood runs the body.
"$BIN" -c 'echo ${ echo body; }'
"$BIN" --mood sh -c 'echo ${ echo body; }' 2>&1 | head -1
echo "rc=$?"
