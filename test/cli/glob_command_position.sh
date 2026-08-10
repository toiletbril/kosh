unset KOSH_FLAGS
# A glob in command position is rejected in the kosh mood and downgraded to a
# warning in a compatibility mood. The test command [ and a quoted glob are not
# globs in command position and stay unflagged.
echo "== kosh mood rejects a command-position glob (count):"
"$BIN" -c '*.zzz_no_such' 2>&1 | grep -c "glob pattern in command position"
echo "== bash mood warns once, not fatal (count):"
"$BIN" -M bash -c '*.zzz_no_such_qqq' 2>&1 | grep -c "warning: A glob pattern in command position"
echo "== a plain command is unaffected:"
"$BIN" -c 'echo plain-ok' 2>&1
echo "== the [ test command is not flagged (count):"
"$BIN" -c '[ -n x ] && echo bracket-ok' 2>&1 | grep -c "command position"
echo "== a quoted glob is not a command-position glob (count):"
"$BIN" -c '"*.zzz" 2>/dev/null; true' 2>&1 | grep -c "command position"
