unset KOSH_FLAGS
# A long flag matches its whole name, so --helpXYZ is unknown rather than a
# prefix match on --help. The caret column tracks the binary path in argv[0],
# so the location prefix is stripped to keep the golden stable across the rel
# and the dbg binary names.
echo "== a long flag matches its whole name, not a prefix:"
"$BIN" --helpXYZ -c 'echo unreached' 2>&1 | head -1 |
  sed 's/^kosh: [0-9]*:[0-9]*: //'
