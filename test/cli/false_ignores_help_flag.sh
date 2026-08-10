unset KOSH_FLAGS
# The false builtin ignores --help and returns 1, the way bash never prints
# help for false and always fails.
echo "== false ignores --help and returns 1:"
"$BIN" -c 'false --help; echo "rc=$?"'
