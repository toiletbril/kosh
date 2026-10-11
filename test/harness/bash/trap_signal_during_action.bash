#!/bin/bash

# Two signals that are queued before one drain run both actions in signal
# number order. A jump the first action leaves ends the drain and drops the
# arrival that is still queued, and a later arrival of the same signal fires
# again. A signal that arrives while an action is running is drained at the
# next boundary inside that action. The status and PIPESTATUS of the command
# the drain interrupted are restored around it.

echo two-signals-one-drain
trap 'echo action-usr1' USR1
trap 'echo action-usr2' USR2
/bin/sh -c "kill -USR1 $$; kill -USR2 $$"
echo "after-two-signals=$?"
trap - USR1
trap - USR2
echo two-signals-one-drain-done

echo pipestatus-across-full-drain
trap 'echo full-usr1; false | true' USR1
trap 'echo full-usr2; true | false' USR2
false | /bin/sh -c "kill -USR1 $$; kill -USR2 $$" | false
echo "pipestatus-full=${PIPESTATUS[*]}"
trap - USR1
trap - USR2
echo pipestatus-across-full-drain-done

echo drain-breaks-on-return
drain_return_target() {
  /bin/sh -c "kill -USR1 $$; kill -USR2 $$"
  echo unreachable-after-kill
  return 2
}
trap 'echo break-usr1; return 6' USR1
trap 'echo break-usr2' USR2
drain_return_target
echo "drain-return-status=$?"
echo after-drain-return
echo second-usr2
kill -USR2 $$
echo after-second-usr2
trap - USR1
trap - USR2
echo drain-breaks-on-return-done

echo pipestatus-across-broken-drain
pipestatus_target() {
  false | /bin/sh -c "kill -USR1 $$; kill -USR2 $$" | false
  echo unreachable-after-pipestatus-kill
  return 2
}
trap 'echo pipe-usr1; true | true; return 6' USR1
trap 'echo pipe-usr2' USR2
pipestatus_target 2> /dev/null
echo "pipestatus-broken=${PIPESTATUS[*]}"
trap - USR1
trap - USR2
echo pipestatus-across-broken-drain-done

echo drain-breaks-on-exit
( trap 'echo exit-usr1; exit 4' USR1
  trap 'echo exit-usr2' USR2
  /bin/sh -c "kill -USR1 $BASHPID; kill -USR2 $BASHPID"
  echo unreachable-after-exit-kill )
echo "drain-exit-status=$?"
echo drain-breaks-on-exit-done

echo queued-signal-stays-queued-under-a-pipeline
queued_target() {
  /bin/sh -c "kill -USR1 $$; kill -USR2 $$"
  echo unreachable-after-queued-kill
}
trap 'echo queued-usr1-head; true | true; echo queued-usr1-tail; return 6' USR1
trap 'echo queued-usr2' USR2
queued_target
echo "queued-status=$?"
echo after-queued
trap - USR1
trap - USR2
echo queued-signal-stays-queued-under-a-pipeline-done

echo signal-sent-from-inside-an-action
trap 'echo sent-usr1-head; kill -USR2 $$; true | true; echo sent-usr1-tail' USR1
trap 'echo sent-usr2' USR2
kill -USR1 $$
echo "sent-status=$?"
trap - USR1
trap - USR2
echo signal-sent-from-inside-an-action-done

echo signal-during-a-debug-action
( set -T
  trap 'echo debug-usr1' USR1
  debug_count=0
  trap 'debug_count=$((debug_count + 1)); if [ $debug_count = 2 ]; then kill -USR1 $BASHPID; true | true; echo debug-action-tail; fi' DEBUG
  echo debug-one
  echo debug-two
  trap - DEBUG
  trap - USR1 )
echo "signal-during-a-debug-action-status=$?"

echo signal-during-an-exit-action
( trap 'echo exit-head; kill -USR1 $BASHPID; true | true; echo exit-tail' EXIT
  trap 'echo exit-action-usr1' USR1
  exit 3 )
echo "signal-during-an-exit-action-status=$?"

echo signal-during-an-err-action
( set -o errtrace
  trap 'echo err-action-usr1' USR1
  trap 'echo err-head; kill -USR1 $BASHPID; true | true; echo err-tail' ERR
  false
  echo after-err-false )
echo "signal-during-an-err-action-status=$?"

echo signal-during-a-return-action
( set -T
  trap 'echo return-action-usr1' USR1
  return_target() { echo return-body; }
  trap 'echo return-head; kill -USR1 $BASHPID; true | true; echo return-tail' RETURN
  return_target
  trap - RETURN
  echo after-return-target )
echo "signal-during-a-return-action-status=$?"

echo trap-signal-during-action-done
