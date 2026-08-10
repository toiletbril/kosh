unset KOSH_FLAGS
"$BIN" --mood bash -c 'sleep 3 & p=$!; kill -STOP "$p"; sleep 0.2; jobs >/dev/null; jobs; kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null' 2>&1 | grep -c Stopped
