unset KOSH_FLAGS
# The <<< here-string works in the default mood and names itself a bashism
# when POSIX mode refuses it.
"$BIN" -c 'read -r x <<< "hello words"; echo "default=$x"'
"$BIN" --mood sh -c 'read -r x <<< hi' 2>&1 | head -1
echo "rc=$?"
