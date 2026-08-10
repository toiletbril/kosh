unset KOSH_FLAGS
# A re-read arithmetic variable is not folded stale, so a later $((n)) sees the
# side effect of an earlier $((n++)).
echo "== a re-read arithmetic variable is not folded stale after a side effect:"
"$BIN" -c 'n=5; echo $((n++)) $((n)) $((n))'
