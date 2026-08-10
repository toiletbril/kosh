unset KOSH_FLAGS
# A non-interactive script stuck in a non-forked builtin loop must terminate on
# SIGTERM and SIGHUP the way dash and bash scripts do. The interactive shell
# blocks those terminal signals, but a script leaves them at their default.
check_signal() {
  signal_name="$1"
  "$BIN" -c 'while :; do :; done' &
  loop_pid=$!
  # The macOS driver shell reports a SIGHUP-killed background job with a Hangup
  # line, so the job is disowned to keep that diagnostic out of the output.
  disown "$loop_pid" 2>/dev/null
  sleep 0.5
  kill -"$signal_name" "$loop_pid" 2>/dev/null
  waited=0
  while [ "$waited" -lt 50 ]; do
    kill -0 "$loop_pid" 2>/dev/null || break
    sleep 0.1
    waited=$((waited + 1))
  done
  if kill -0 "$loop_pid" 2>/dev/null; then
    kill -KILL "$loop_pid" 2>/dev/null
    echo "$signal_name: still alive"
  else
    echo "$signal_name: terminated"
  fi
}
echo "== SIGTERM terminates a non-interactive loop:"
check_signal TERM
echo "== SIGHUP terminates a non-interactive loop:"
check_signal HUP
