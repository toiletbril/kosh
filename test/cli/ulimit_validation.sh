unset KOSH_FLAGS

if [ "${OS-}" = Windows_NT ]; then
  output=$("$BIN" -c 'ulimit -a' 2>&1)
  status=$?
  case "$output:$status" in
    *"Unable to read resource limits"*:1) echo all=ok ;;
    *) echo all=broken ;;
  esac
else
  "$BIN" -c 'ulimit -a' >/dev/null 2>&1 && echo all=ok || echo all=broken
fi

output=$("$BIN" -c 'ulimit -p; ulimit -p 9' 2>/dev/null)
status=$?
if [ "$output" = 8 ] && [ "$status" -eq 1 ]; then
  echo pipe=ok
else
  echo pipe=broken
fi

[ "${OS-}" = Windows_NT ] && exit

for value in banana -1 1x 18446744073709551616 ''
do
  "$BIN" -c 'before=$(ulimit -n); ulimit -n -- "$1" >/dev/null 2>&1; status=$?; after=$(ulimit -n); printf "value=<%s> status=%s unchanged=%s\n" "$1" "$status" "$([ "$before" = "$after" ] && printf yes)"' kosh "$value"
done
