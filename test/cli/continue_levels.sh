unset KOSH_FLAGS
# continue rejects a count below one, defaults to one enclosing loop, and a count
# above one skips to the next iteration of an outer loop.
echo "== a zero count is rejected:"
"$BIN" -c 'for i in 1 2; do continue 0; done'; echo "rc=$?"
echo "== a bare continue skips the rest of the body:"
"$BIN" -c 'for i in 1 2 3; do continue; echo never; done; echo done'
echo "== a count of two skips to the outer loop:"
"$BIN" -c 'for i in a b; do for j in x y; do continue 2; echo never; done; echo inner_never; done; echo outer_done'
