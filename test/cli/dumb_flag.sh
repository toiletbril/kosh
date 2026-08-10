unset KOSH_FLAGS
"$BIN" --dumb -c 'echo dumb-ok'
echo "rc=$?"
"$BIN" -V >/dev/null
echo "version-rc=$?"
