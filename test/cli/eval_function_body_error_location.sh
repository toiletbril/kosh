unset KOSH_FLAGS
# An error raised inside a function defined by eval and called after the eval has
# returned renders against the stored body copy, with the eval origin and the
# caret on the body. Before the fix the bash mood rendered the error against the
# top-level eval line instead of the body, and an eval of a variable did the
# same. The arithmetic stays escaped so the division fires inside the function
# body at call time rather than during the eval argument expansion.
echo "== default mood, literal eval argument =="
{
    "$BIN" -c 'eval "f(){ echo \$((1/0)); }"; f' 2>&1
    printf 'rc=%s\n' "$?"
} | ./normalize-trace.sh "$BIN"
echo "== bash mood, literal eval argument =="
{
    "$BIN" --mood bash -c 'eval "g(){ echo \$((1/0)); }"; g' 2>&1
    printf 'rc=%s\n' "$?"
} | ./normalize-trace.sh "$BIN"
echo "== default mood, eval of a variable =="
{
    "$BIN" -c 'code=$(printf "%s" "h(){ echo \$((1/0)); }"); eval "$code"; h' 2>&1
    printf 'rc=%s\n' "$?"
} | ./normalize-trace.sh "$BIN"
echo "== bash mood, function keyword form =="
{
    "$BIN" --mood bash -c 'eval "function k { echo \$((1/0)); }"; k' 2>&1
    printf 'rc=%s\n' "$?"
} | ./normalize-trace.sh "$BIN"
echo "== function defined in default mood, called from bash, renders once =="
"$BIN" -c 'm(){ echo $((1/0)); }; set --mood bash; m' 2>&1 | grep -c division
