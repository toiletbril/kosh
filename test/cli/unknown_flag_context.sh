unset KOSH_FLAGS
# Unknown flag errors carry the program context. A builtin reports the
# 'Builtin name' prefix and a koshkit utility reports the 'koshkit util'
# prefix, with the caret staying on the flag.
echo "== builtin unknown flag carries the Builtin prefix:"
"$BIN" -c 'enable --badflag' 2>&1
echo "== koshkit unknown flag carries the koshkit util prefix:"
"$BIN" -c 'koshkit ls --dasdas' 2>&1
