unset KOSH_FLAGS
echo "== disown %1 leaves one running job:"
"$BIN" --mood bash -c 'sleep 2 & sleep 2 & disown %1; jobs' 2>&1 | grep -c Running
echo "== disown -a leaves none:"
"$BIN" --mood bash -c 'sleep 2 & sleep 2 & disown -a; jobs; echo END' 2>&1 | grep -c Running
echo "== bare disown drops the most recent job:"
"$BIN" --mood bash -c 'sleep 2 & disown; jobs; echo END' 2>&1 | grep -c Running
echo "== an unknown job id is an error:"
"$BIN" --mood bash -c 'disown %99' 2>&1 | grep -c "not a valid job"
