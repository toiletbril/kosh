#!/bin/bash

GOLDEN=$1
TIMEOUT_SECONDS=${CLI_TEST_TIMEOUT_SECONDS:-60}
GOLDEN_PROCESS=
GOLDEN_SESSION=
HOST_SYSTEM=$(uname -s)
CLEANUP_IS_ARMED=yes
PENDING_EXIT_STATUS=

case $TIMEOUT_SECONDS in
''|*[!0-9]*|0)
  printf 'invalid CLI golden timeout\n'
  exit 125
  ;;
esac

list_golden_session_processes()
{
  if [ "$HOST_SYSTEM" = Linux ]; then
    for PROCESS_STAT_PATH in /proc/[0-9]*/stat; do
      [ -r "$PROCESS_STAT_PATH" ] || continue
      IFS= read -r PROCESS_STAT 2>/dev/null < "$PROCESS_STAT_PATH" || continue
      PROCESS_FIELDS=${PROCESS_STAT##*) }
      set -- $PROCESS_FIELDS
      [ "$#" -ge 4 ] || continue
      [ "$1" = Z ] && continue
      [ "$4" = "$GOLDEN_SESSION" ] || continue
      PROCESS_ID=${PROCESS_STAT_PATH#/proc/}
      printf '%s\n' "${PROCESS_ID%/stat}"
    done
    return
  fi

  if [ "$HOST_SYSTEM" = Darwin ]; then
    PROCESS_IDS=$(ps -Ao pid= 2>/dev/null) || return 1
    printf '%s\n' "$PROCESS_IDS" | python3 -c '
import os
import sys

session = int(sys.argv[1])
try:
  os.getsid(0)
except (AttributeError, OSError):
  raise SystemExit(1)
for line in sys.stdin:
  process = int(line)
  try:
    process_session = os.getsid(process)
  except OSError:
    continue
  if process_session == session:
    print(process)
    ' "$GOLDEN_SESSION"
    return
  fi

  if PROCESS_TABLE=$(ps -Ao pid=,sess= 2>/dev/null); then
    :
  elif PROCESS_TABLE=$(ps -Ao pid=,sid= 2>/dev/null); then
    :
  else
    return 1
  fi
  printf '%s\n' "$PROCESS_TABLE" | while read -r PROCESS_ID PROCESS_SESSION; do
    if [ "$PROCESS_SESSION" = "$GOLDEN_SESSION" ]; then
      printf '%s\n' "$PROCESS_ID"
    fi
  done
}

terminate_golden_tree()
{
  if [ "${OS-}" = Windows_NT ] &&
    command -v taskkill.exe >/dev/null 2>&1; then
    [ -n "$GOLDEN_PROCESS" ] || return
    taskkill.exe //PID "$GOLDEN_PROCESS" //T //F >/dev/null 2>&1 || true
    kill -KILL "$GOLDEN_PROCESS" 2>/dev/null || true
    return
  fi

  DISCOVERY_FAILED=no
  SESSION_PROCESSES=$(list_golden_session_processes) || {
    DISCOVERY_FAILED=yes
    SESSION_PROCESSES=
  }
  if [ -n "$GOLDEN_PROCESS$SESSION_PROCESSES" ]; then
    kill -TERM $GOLDEN_PROCESS $SESSION_PROCESSES 2>/dev/null || true
  fi
  sleep 0.1
  SESSION_PROCESSES=$(list_golden_session_processes) || {
    DISCOVERY_FAILED=yes
    SESSION_PROCESSES=
  }
  if [ -n "$GOLDEN_PROCESS$SESSION_PROCESSES" ]; then
    kill -KILL $GOLDEN_PROCESS $SESSION_PROCESSES 2>/dev/null || true
  fi
  [ "$DISCOVERY_FAILED" = no ]
}

cleanup_golden_tree()
{
  if [ "$CLEANUP_IS_ARMED" = yes ] && [ -n "$GOLDEN_SESSION" ]; then
    if ! terminate_golden_tree; then
      printf 'golden session discovery failed\n'
    fi
    if [ -n "$GOLDEN_PROCESS" ]; then
      wait "$GOLDEN_PROCESS" 2>/dev/null || true
    fi
  fi
}

request_exit()
{
  PENDING_EXIT_STATUS=$1
  if [ -n "$GOLDEN_SESSION" ]; then
    exit "$PENDING_EXIT_STATUS"
  fi
}

trap cleanup_golden_tree EXIT
trap 'request_exit 130' INT
trap 'request_exit 143' TERM
trap 'request_exit 129' HUP

if [ "${OS-}" = Windows_NT ]; then
  BIN=$BIN BOUNDED_GOLDEN=$GOLDEN BOUNDED_TIMEOUT_SECONDS=$TIMEOUT_SECONDS \
    KOSH_TEST_TIMEOUT_JOB_LIFETIME=leader \
    "$BIN" -p --mood sh -c \
    'koshkit timeout "$BOUNDED_TIMEOUT_SECONDS" "$BIN" --mood sh -c '\''unset KOSH_TEST_TIMEOUT_JOB_LIFETIME; sh "$BOUNDED_GOLDEN"'\''' &
elif command -v setsid >/dev/null 2>&1; then
  BIN=$BIN setsid /bin/sh "$GOLDEN" &
elif command -v perl >/dev/null 2>&1; then
  BIN=$BIN perl -MPOSIX -e 'POSIX::setsid(); exec @ARGV' \
    /bin/sh "$GOLDEN" &
else
  printf 'cannot create a CLI golden session\n'
  exit 125
fi
GOLDEN_PROCESS=$!
GOLDEN_SESSION=$GOLDEN_PROCESS
if [ -n "$PENDING_EXIT_STATUS" ]; then
  exit "$PENDING_EXIT_STATUS"
fi

if [ "${OS-}" = Windows_NT ]; then
  wait "$GOLDEN_PROCESS"
  GOLDEN_STATUS=$?
  GOLDEN_PROCESS=
  CLEANUP_IS_ARMED=no
  if [ "$GOLDEN_STATUS" -eq 124 ]; then
    printf 'golden timed out\n'
  fi
  exit "$GOLDEN_STATUS"
fi

ATTEMPT_COUNT=0
ATTEMPT_LIMIT=$((TIMEOUT_SECONDS * 10))
while kill -0 "$GOLDEN_PROCESS" 2>/dev/null &&
  [ "$ATTEMPT_COUNT" -lt "$ATTEMPT_LIMIT" ]; do
  sleep 0.1
  ATTEMPT_COUNT=$((ATTEMPT_COUNT + 1))
done

if kill -0 "$GOLDEN_PROCESS" 2>/dev/null; then
  printf 'golden timed out\n'
  TERMINATION_STATUS=0
  terminate_golden_tree || TERMINATION_STATUS=$?
  wait "$GOLDEN_PROCESS" 2>/dev/null || true
  GOLDEN_PROCESS=
  CLEANUP_IS_ARMED=no
  if [ "$TERMINATION_STATUS" -ne 0 ]; then
    printf 'golden session discovery failed\n'
    exit 125
  fi
  exit 124
fi

wait "$GOLDEN_PROCESS"
GOLDEN_STATUS=$?
GOLDEN_PROCESS=
ATTEMPT_COUNT=0
HAS_LIVING_DESCENDANT=yes
SESSION_DISCOVERY_FAILED=no
while [ "$HAS_LIVING_DESCENDANT" = yes ] && [ "$ATTEMPT_COUNT" -lt 1000 ]; do
  HAS_LIVING_DESCENDANT=no
  if [ "${OS-}" != Windows_NT ]; then
    SESSION_PROCESSES=$(list_golden_session_processes) || {
      SESSION_DISCOVERY_FAILED=yes
      break
    }
    for PROCESS_ID in $SESSION_PROCESSES; do
      if [ -n "$PROCESS_ID" ]; then
        HAS_LIVING_DESCENDANT=yes
        break
      fi
    done
  fi
  if [ "$HAS_LIVING_DESCENDANT" = yes ]; then
    sleep 0.01
    ATTEMPT_COUNT=$((ATTEMPT_COUNT + 1))
  fi
done
if [ "$SESSION_DISCOVERY_FAILED" = yes ]; then
  printf 'golden session discovery failed\n'
  terminate_golden_tree 2>/dev/null || true
  CLEANUP_IS_ARMED=no
  exit 125
fi
if [ "$HAS_LIVING_DESCENDANT" = yes ]; then
  printf 'golden leaked processes\n'
  terminate_golden_tree
  CLEANUP_IS_ARMED=no
  exit 125
fi

CLEANUP_IS_ARMED=no
exit "$GOLDEN_STATUS"
