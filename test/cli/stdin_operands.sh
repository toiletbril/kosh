unset KOSH_FLAGS
printf 'echo "got $1 and $2"' | "$BIN" - alpha beta
printf 'echo "dd"' | "$BIN" -- -
echo "rc=$?"
