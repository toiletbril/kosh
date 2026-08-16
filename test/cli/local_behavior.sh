unset KOSH_FLAGS
export KOSH_FLAGS=--no-diagnostics
# local declares a function-scoped variable and rejects use outside a function.
# The attribute flags declare an integer, an indexed array, and an associative
# array, the same letters declare takes.
echo "== local outside a function is an error:"
{
    "$BIN" -c 'local x=5' 2>&1
    printf 'rc=%s\n' "$?"
} | ./normalize-trace.sh "$BIN"
echo "== local -i evaluates an integer assignment:"
"$BIN" -c 'f(){ local -i n=3+4; echo $n; }; f'
echo "== local -a declares an indexed array:"
"$BIN" -c 'f(){ local -a a; a[0]=x; a[1]=y; echo "${a[0]}${a[1]}"; }; f'
echo "== local -A declares an associative array:"
"$BIN" -c 'f(){ local -A m; m[key]=val; echo "${m[key]}"; }; f'
echo "== a local shadows an outer value and restores it:"
"$BIN" -c 'v=outer; f(){ local v=inner; echo $v; }; f; echo $v'
echo "== invalid local names fail without creating hidden bindings:"
"$BIN" -c 'f(){
  local bad-name "" 3name 2>/dev/null
  echo "status=$?"
  declare -p bad-name >/dev/null 2>&1
  echo "hidden=$?"
}; f'

unset KOSH_FLAGS
# The no-local warning fires for a fresh name assigned in a function body, the
# leaking footgun, but stays quiet for a name already set at the top level or
# inherited from the environment, since that assignment updates an existing
# variable rather than leaking a new one.
echo "== fresh name warns:"
"$BIN" -W -c 'fn(){ brandnewname=1; }; fn' 2>&1 | grep -c 'has no .local.'
echo "== top-level name is quiet:"
"$BIN" -W -c 'seeded=0; fn(){ seeded=1; }; fn' 2>&1 | grep -c 'has no .local.'
echo "== inherited PATH is quiet:"
"$BIN" -W -c 'fn(){ PATH="${PATH:+$PATH$TEST_PATH_SEPARATOR}/x"; }; fn' 2>&1 | grep -c 'has no .local.'
echo "rc=$?"
