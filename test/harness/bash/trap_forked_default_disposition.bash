#!/bin/bash
# A forked child gets the default action back for every signal the parent
# traps with an action, checked against bash. An ignored signal stays ignored.
usr1_number=$(kill -l USR1)
killed_status=$((128 + usr1_number))

report() {
  if [ "$2" -eq "$killed_status" ]; then
    echo "$1=killed"
  else
    echo "$1=$2"
  fi
}

trap 'echo caught_usr1' USR1

( kill -USR1 $BASHPID; echo subshell_alive )
report subshell $?

{ kill -USR1 $BASHPID; echo async_alive; } &
wait $!
report async $?

{ kill -USR1 $BASHPID; echo stage_alive; } | /bin/cat
report stage "${PIPESTATUS[0]}"

coproc CP { read -r _; kill -USR1 $BASHPID; echo coproc_alive; }
coproc_pid=$CP_PID
echo go >&"${CP[1]}"
wait "$coproc_pid"
report coproc $?

echo "--- the parent still holds its own action"
kill -USR1 $$
echo "parent_alive=$?"

trap - USR1

echo "--- an ignored signal stays ignored in the child"
trap '' USR2
( kill -USR2 $BASHPID; echo ignored_alive )
report ignored $?
trap - USR2

echo done
