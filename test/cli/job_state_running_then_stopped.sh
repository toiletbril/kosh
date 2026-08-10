unset KOSH_FLAGS
# A background job reads Running, then Stopped after a SIGSTOP, and the Stopped
# state survives a later poll rather than flipping back to Running.
"$BIN" --mood bash -c 'sleep 5 & p=$!; jobs; kill -STOP "$p"; sleep 0.2; jobs >/dev/null; jobs; kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null' 2>&1 | grep -oE "Running|Stopped" | tr '\n' ' '
echo ""
