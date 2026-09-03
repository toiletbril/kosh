#!/bin/sh

"$BIN" --debug-arena-lifetimes

report=$("$BIN" --show-memory -c '
i=0
while [ "$i" -lt 256 ]; do
  eval "function_redefinition_probe() { : $i; }"
  i=$((i + 1))
done
' 2>&1)
function_line=$(printf '%s\n' "$report" | grep '^Function arenas:')
case $function_line in
  *'reserved 16384, blocks 1, '*) echo 'function-redefinition=bounded' ;;
  *) echo 'function-redefinition=unbounded' ;;
esac

report=$("$BIN" --show-memory -c '
i=0
while [ "$i" -lt 256 ]; do
  eval "function_release_probe() { : $i; }"
  i=$((i + 1))
done
unset -f function_release_probe
' 2>&1)
function_line=$(printf '%s\n' "$report" | grep '^Function arenas:')
case $function_line in
  *'used 0, reserved 0, blocks 0, destructors 0 of 0'*)
    echo 'function-unset=released'
    ;;
  *)
    echo 'function-unset=retained'
    ;;
esac
