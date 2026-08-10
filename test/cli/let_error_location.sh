unset SHIT_FLAGS
echo "== default argument location =="
"$BIN" -c 'a=0; let a = 2; echo unreached' 2>&1
echo "rc=$?"
echo "== bash soft argument location =="
"$BIN" --mood bash -c 'a=0; let a = 2; echo "status=$? after"' 2>&1
echo "rc=$?"
echo "== later argument location =="
"$BIN" --mood bash -c 'let "first=1" "bad=1 +" "later=3"; echo "status=$? later=${later-unset}"' 2>&1
echo "rc=$?"
echo "== diagnostic catalog =="
if "$BIN" --list-diagnostics | grep -q SC2219; then
  echo "SC2219=present"
else
  echo "SC2219=absent"
fi
"$BIN" --list-diagnostics | grep -c arith-assign
catalog=$("$BIN" --list-diagnostics)
echo "SC2024-variants=$(printf '%s\n' "$catalog" |
  grep -c '^  SC2024  strict  sudo-')"
echo "SC2184-variants=$(printf '%s\n' "$catalog" |
  grep -c '^  SC2184  strict  unquoted-unset-index$')"
echo "exported-cdpath=$(printf '%s\n' "$catalog" |
  grep -c '^  exported-cdpath  strict$')"
echo "catalog-nounset=$(printf '%s\n' "$catalog" |
  grep -c '^  nounset  ')"
