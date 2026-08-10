unset KOSH_FLAGS
# disown -r drops the running jobs and keeps a stopped one. It refreshes the
# recorded state itself, so a stopped job survives even with no jobs call before
# the disown. The short wait lets the stop reach the job before disown runs.
echo "== one stopped job survives disown -r:"
"$BIN" --mood bash -c 'sleep 5 & q=$!; sleep 5 & p=$!; kill -STOP "$p"; sleep 0.2; disown -r; jobs; kill "$q" 2>/dev/null; kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null' 2>&1 | grep -c Stopped
echo "== and no running job remains after disown -r:"
"$BIN" --mood bash -c 'sleep 5 & q=$!; sleep 5 & p=$!; kill -STOP "$p"; sleep 0.2; disown -r; jobs; kill "$q" 2>/dev/null; kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null' 2>&1 | grep -c Running
