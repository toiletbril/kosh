unset KOSH_FLAGS
# bash brace-expands before lexing a variable name, so a greedy $name that a
# brace expansion leaves next to name bytes absorbs them. A bounded ${name} is
# left split.
echo "== a greedy \$name merges with the brace postamble:"
"$BIN" --mood bash -c 'foo=X; foobar=MERGED; echo {$foo,b}bar'
echo "== the merge stops at a non-name byte:"
"$BIN" --mood bash -c 'foo=X; echo {$foo,b}_end.tail'
echo "== a bounded \${name} is left split, not merged:"
"$BIN" --mood bash -c 'foo=X; foobar=MERGED; echo ${foo}bar'
echo "== a plain brace group without a variable is unchanged:"
"$BIN" --mood bash -c 'echo {a,b}c'
