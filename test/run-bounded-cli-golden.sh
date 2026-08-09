#!/bin/bash

golden=$1
timeout_seconds=${CLI_TEST_TIMEOUT_SECONDS:-60}
golden_process=
golden_session=
host_system=$(uname -s)
cleanup_is_armed=yes
pending_exit_status=

case $timeout_seconds in
''|*[!0-9]*|0)
  printf 'invalid CLI golden timeout\n'
  exit 125
  ;;
esac

list_golden_session_processes()
{
  if [ "$host_system" = Linux ]; then
    for process_stat_path in /proc/[0-9]*/stat; do
      [ -r "$process_stat_path" ] || continue
      IFS= read -r process_stat < "$process_stat_path" || continue
      process_fields=${process_stat##*) }
      set -- $process_fields
      [ "$#" -ge 4 ] || continue
      [ "$1" = Z ] && continue
      [ "$4" = "$golden_session" ] || continue
      process_id=${process_stat_path#/proc/}
      printf '%s\n' "${process_id%/stat}"
    done
    return
  fi

  if [ "$host_system" = Darwin ]; then
    process_ids=$(ps -Ao pid= 2>/dev/null) || return 1
    printf '%s\n' "$process_ids" | python3 -c '
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
    ' "$golden_session"
    return
  fi

  if process_table=$(ps -Ao pid=,sess= 2>/dev/null); then
    :
  elif process_table=$(ps -Ao pid=,sid= 2>/dev/null); then
    :
  else
    return 1
  fi
  printf '%s\n' "$process_table" | while read -r process_id process_session; do
    if [ "$process_session" = "$golden_session" ]; then
      printf '%s\n' "$process_id"
    fi
  done
}

terminate_golden_tree()
{
  if [ "${OS-}" = Windows_NT ] &&
    command -v taskkill.exe >/dev/null 2>&1; then
    [ -n "$golden_process" ] || return
    taskkill.exe //PID "$golden_process" //T //F >/dev/null 2>&1 || true
    kill -KILL "$golden_process" 2>/dev/null || true
    return
  fi

  discovery_failed=no
  session_processes=$(list_golden_session_processes) || {
    discovery_failed=yes
    session_processes=
  }
  if [ -n "$golden_process$session_processes" ]; then
    kill -TERM $golden_process $session_processes 2>/dev/null || true
  fi
  sleep 0.1
  session_processes=$(list_golden_session_processes) || {
    discovery_failed=yes
    session_processes=
  }
  if [ -n "$golden_process$session_processes" ]; then
    kill -KILL $golden_process $session_processes 2>/dev/null || true
  fi
  [ "$discovery_failed" = no ]
}

cleanup_golden_tree()
{
  if [ "$cleanup_is_armed" = yes ] && [ -n "$golden_session" ]; then
    if ! terminate_golden_tree; then
      printf 'golden session discovery failed\n'
    fi
    if [ -n "$golden_process" ]; then
      wait "$golden_process" 2>/dev/null || true
    fi
  fi
}

request_exit()
{
  pending_exit_status=$1
  if [ -n "$golden_session" ]; then
    exit "$pending_exit_status"
  fi
}

trap cleanup_golden_tree EXIT
trap 'request_exit 130' INT
trap 'request_exit 143' TERM
trap 'request_exit 129' HUP

if [ "${OS-}" = Windows_NT ]; then
  BIN=$BIN BOUNDED_GOLDEN=$golden BOUNDED_TIMEOUT_SECONDS=$timeout_seconds \
    SHIT_TEST_TIMEOUT_JOB_LIFETIME=leader \
    "$BIN" -p --mood sh -c \
    'shitbox timeout "$BOUNDED_TIMEOUT_SECONDS" "$BIN" --mood sh -c '\''unset SHIT_TEST_TIMEOUT_JOB_LIFETIME; sh "$BOUNDED_GOLDEN"'\''' &
elif command -v setsid >/dev/null 2>&1; then
  BIN=$BIN setsid /bin/sh "$golden" &
elif command -v perl >/dev/null 2>&1; then
  BIN=$BIN perl -MPOSIX -e 'POSIX::setsid(); exec @ARGV' \
    /bin/sh "$golden" &
else
  printf 'cannot create a CLI golden session\n'
  exit 125
fi
golden_process=$!
golden_session=$golden_process
if [ -n "$pending_exit_status" ]; then
  exit "$pending_exit_status"
fi

if [ "${OS-}" = Windows_NT ]; then
  wait "$golden_process"
  golden_status=$?
  golden_process=
  cleanup_is_armed=no
  if [ "$golden_status" -eq 124 ]; then
    printf 'golden timed out\n'
  fi
  exit "$golden_status"
fi

attempt_count=0
attempt_limit=$((timeout_seconds * 10))
while kill -0 "$golden_process" 2>/dev/null &&
  [ "$attempt_count" -lt "$attempt_limit" ]; do
  sleep 0.1
  attempt_count=$((attempt_count + 1))
done

if kill -0 "$golden_process" 2>/dev/null; then
  printf 'golden timed out\n'
  termination_status=0
  terminate_golden_tree || termination_status=$?
  wait "$golden_process" 2>/dev/null || true
  golden_process=
  cleanup_is_armed=no
  if [ "$termination_status" -ne 0 ]; then
    printf 'golden session discovery failed\n'
    exit 125
  fi
  exit 124
fi

wait "$golden_process"
golden_status=$?
golden_process=
attempt_count=0
has_living_descendant=yes
session_discovery_failed=no
while [ "$has_living_descendant" = yes ] && [ "$attempt_count" -lt 1000 ]; do
  has_living_descendant=no
  if [ "${OS-}" != Windows_NT ]; then
    session_processes=$(list_golden_session_processes) || {
      session_discovery_failed=yes
      break
    }
    for process_id in $session_processes; do
      if [ -n "$process_id" ]; then
        has_living_descendant=yes
        break
      fi
    done
  fi
  if [ "$has_living_descendant" = yes ]; then
    sleep 0.01
    attempt_count=$((attempt_count + 1))
  fi
done
if [ "$session_discovery_failed" = yes ]; then
  printf 'golden session discovery failed\n'
  terminate_golden_tree 2>/dev/null || true
  cleanup_is_armed=no
  exit 125
fi
if [ "$has_living_descendant" = yes ]; then
  printf 'golden leaked processes\n'
  terminate_golden_tree
  cleanup_is_armed=no
  exit 125
fi

cleanup_is_armed=no
exit "$golden_status"
