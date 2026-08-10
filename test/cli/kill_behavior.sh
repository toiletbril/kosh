unset KOSH_FLAGS
# The kill builtin and the killall and pkill utilities list the signal names
# under -l and --list, all through one shared formatter that reads the platform
# signal table.
echo "== kill -l lists SIGTERM (count):"
"$BIN" -c 'kill -l' 2>&1 | grep -c ') SIGTERM'
echo "== kill --list lists SIGKILL (count):"
"$BIN" -c 'kill --list' 2>&1 | grep -c ') SIGKILL'
echo "== killall -l lists SIGHUP (count):"
"$BIN" -c 'koshkit killall -l' </dev/null 2>&1 | grep -c ') SIGHUP'
echo "== pkill -l lists SIGINT (count):"
"$BIN" -c 'koshkit pkill -l' </dev/null 2>&1 | grep -c ') SIGINT'
echo "== the list has the common signal set:"
signal_count=$("$BIN" -c 'kill -l' 2>&1 | grep -cE '^[0-9]+\) SIG')
test "$signal_count" -ge 5 && echo complete

unset KOSH_FLAGS
# kill rejects a missing target, an unknown signal name, an unknown job, and a
# non-numeric target rather than falling through to a process-group signal. No
# real signal is delivered, every case is an error path with a fixed message.
echo "== no target is required:"; "$BIN" -c 'kill'; echo "rc=$?"
echo "== an unknown signal name is rejected:"; "$BIN" -c 'kill -BOGUSSIG 123'; echo "rc=$?"
echo "== an unknown job is reported:"; "$BIN" -c 'kill %999'; echo "rc=$?"
echo "== a non-numeric job is rejected:"; "$BIN" -c 'kill %abc'; echo "rc=$?"
echo "== a non-numeric pid is rejected:"; "$BIN" -c 'kill notanumber'; echo "rc=$?"
echo "== signal zero probes the current process:"
"$BIN" -c 'kill -s 0 "$$"; echo "named=$?"; kill -n 0 "$$"; echo "numbered=$?"'
echo "== signal zero rejects an exited process:"
"$BIN" -c '"$BIN" -c ":" & child=$!; wait "$child"; kill -0 "$child"; echo "exited=$?"' \
    2>/dev/null
