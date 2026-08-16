unset KOSH_FLAGS

for value in banana -1 1x 18446744073709551616 ''
do
  "$BIN" -c 'before=$(ulimit -n); ulimit -n -- "$1" >/dev/null 2>&1; status=$?; after=$(ulimit -n); printf "value=<%s> status=%s unchanged=%s\n" "$1" "$status" "$([ "$before" = "$after" ] && printf yes)"' kosh "$value"
done
