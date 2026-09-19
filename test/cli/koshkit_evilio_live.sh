#!/bin/sh

run_live_report() {
  live_report_path=$1
  live_command=$2
  : > "$live_report_path"
  set -m
  "$BIN" -c "$live_command" > "$live_report_path" &
  live_pid=$!
  set +m

  live_attempt=0
  live_report=
  while [ "$live_attempt" -lt 250 ]; do
    live_report=$(< "$live_report_path")
    case $live_report in
      *ctrl*c\ to\ exit.*COMMAND*|*ctrl*c\ to\ exit.*RETRIES*) break ;;
    esac
    sleep 0.02
    live_attempt=$((live_attempt + 1))
  done

  if kill -0 "$live_pid" 2> "$TEST_NULL_DEVICE"; then
    kill -INT "$live_pid"
  fi
  wait "$live_pid"
  live_status=$?
  live_report=$(< "$live_report_path")
}

run_live_report "$TEST_TEMP_DIRECTORY/evilio-live-process-report" \
  'koshkit --color never evilio --live=0.05 --cumulative=0.02 --ps -1 --sort pid'
process_live_status=$live_status
process_live_report=$live_report
case $process_live_report in
  *ctrl*c*exit*PID*READ/0.02s*WRITE/0.02s*COMMAND*) live_shape=matched ;;
  *) live_shape=wrong ;;
esac
run_live_report "$TEST_TEMP_DIRECTORY/evilio-live-process-window-report" \
  'koshkit --color never evilio --live=0.05 --cumulative=1.2 --ps -1'
process_window_report=$live_report
case $process_window_report in
  *ctrl*c*exit*"READ OPS/1.2s"*"WRITE OPS/1.2s"*)
    process_window_shape=matched
    ;;
  *) process_window_shape=wrong ;;
esac
case $process_live_report in
  *DEVICE*|*MEMORY*|*SWAP*) live_scope=extra ;;
  *) live_scope=only-process-io ;;
esac

run_live_report "$TEST_TEMP_DIRECTORY/evilio-live-disk-report" \
  'koshkit --color never evilio --live=0.05 --cumulative=0.02 --sort read'
disk_live_status=$live_status
disk_live_report=$live_report
case $disk_live_report in
  *ctrl*c*exit*DEVICE*READ/0.02s*WRITE/0.02s*READ\ OPS/0.02s*WRITE\ OPS/0.02s*BUSY*READ\ LAT*WRITE\ LAT*AVG\ QUEUE*QUEUE*ERRORS*RETRIES*)
    disk_live_shape=matched
    ;;
  *) disk_live_shape=wrong ;;
esac
case $disk_live_report in
  *DISKS*|*MEMORY*|*SWAP*) disk_live_scope=extra ;;
  *) disk_live_scope=only-disk-io ;;
esac
case $disk_live_report in
  *ctrl*c\ to\ exit.*) disk_live_margin=unindented ;;
  *) disk_live_margin=wrong ;;
esac
run_live_report "$TEST_TEMP_DIRECTORY/evilio-live-disk-window-report" \
  'koshkit --color never evilio --live=0.05 --cumulative=1.2'
disk_window_report=$live_report
case $disk_window_report in
  *ctrl*c*exit*"READ OPS/1.2s"*"WRITE OPS/1.2s"*)
    disk_window_shape=matched
    ;;
  *) disk_window_shape=wrong ;;
esac

printf 'status=%s\n' "$process_live_status"
printf 'shape=%s\n' "$live_shape"
printf 'process-window-shape=%s\n' "$process_window_shape"
printf 'scope=%s\n' "$live_scope"
printf 'disk-status=%s\n' "$disk_live_status"
printf 'disk-shape=%s\n' "$disk_live_shape"
printf 'disk-scope=%s\n' "$disk_live_scope"
printf 'disk-margin=%s\n' "$disk_live_margin"
printf 'disk-window-shape=%s\n' "$disk_window_shape"

$BIN -c 'koshkit evilio --live=bad' > /dev/null 2>&1
printf 'rejected-live-status=%s\n' "$?"

help=$($BIN -c 'koshkit evilio --help')
case $help in
  *"Refresh live output"*"0.5 seconds"*) live_help=described ;;
  *) live_help=missing ;;
esac
case $help in
  *"Collect activity over an optional"*"window; the default is one second."*)
    cumulative_help=described
    ;;
  *) cumulative_help=missing ;;
esac
printf 'live-help=%s\n' "$live_help"
printf 'cumulative-help=%s\n' "$cumulative_help"
