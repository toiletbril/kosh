#!/bin/sh

ps_all=$($BIN -c 'koshkit --color never evilps --show-pids --numeric-sort')
case $ps_all in
  *PROCESSES*|*'├── '*|*'└── '*) ps_tree=matched ;;
  *) ps_tree=wrong ;;
esac
printf 'evilps-tree=%s\n' "$ps_tree"

ps_limited=$($BIN -c 'koshkit --color never evilps --show-pids -3')
all_lines=$(printf '%s\n' "$ps_all" | wc -l)
limited_lines=$(printf '%s\n' "$ps_limited" | wc -l)
if [ "$limited_lines" -le "$all_lines" ]; then ps_limit=matched; else ps_limit=wrong; fi
printf 'evilps-limit=%s\n' "$ps_limit"
case $ps_limited in
  *'('*) ps_pid_labels=matched ;;
  *) ps_pid_labels=wrong ;;
esac
printf 'evilps-pid-labels=%s\n' "$ps_pid_labels"

ps_all=$($BIN -c 'koshkit --color never evilps -a -3')
case $ps_all in
  *'['*CPU*MEM*']'*) ps_all_format=matched ;;
  *) ps_all_format=wrong ;;
esac
printf 'evilps-all-format=%s\n' "$ps_all_format"

ps_sorted=$($BIN -c 'koshkit --color never evilps --sort cpu -3')
if [ -n "$ps_sorted" ]; then ps_sorted_roots=matched; else ps_sorted_roots=wrong; fi
printf 'evilps-sorted-roots=%s\n' "$ps_sorted_roots"

evilps_help=$($BIN -c 'koshkit evilps --help')
case $evilps_help in
  *'--live[=<seconds>]'*'--cumulative[=<seconds>]'*)
    evilps_sampling_help=matched
    ;;
  *) evilps_sampling_help=wrong ;;
esac
printf 'evilps-sampling-help=%s\n' "$evilps_sampling_help"

$BIN -c 'koshkit evilps --live=0' > /dev/null 2>&1
printf 'evilps-invalid-live=%s\n' "$?"
$BIN -c 'koshkit evilps --cumulative=0' > /dev/null 2>&1
printf 'evilps-invalid-cumulative=%s\n' "$?"

evilps_live_path=$TEST_TEMP_DIRECTORY/evilps-live-report
set -m
$BIN -c 'koshkit --color never evilps --show-pids --live=0.05 --cumulative=0.1 -1' \
  > "$evilps_live_path" &
evilps_live_pid=$!
set +m
evilps_live_attempt=0
while [ ! -s "$evilps_live_path" ] && [ "$evilps_live_attempt" -lt 250 ]; do
  sleep 0.02
  evilps_live_attempt=$((evilps_live_attempt + 1))
done
sleep 0.3
if kill -0 "$evilps_live_pid" 2> "$TEST_NULL_DEVICE"; then
  kill -INT "$evilps_live_pid"
fi
wait "$evilps_live_pid"
printf 'evilps-live-status=%s\n' "$?"
evilps_live_lines=$(wc -l < "$evilps_live_path")
if [ "$evilps_live_lines" -ge 2 ]; then
  evilps_live_refresh=matched
else
  evilps_live_refresh=wrong
fi
printf 'evilps-live-refresh=%s\n' "$evilps_live_refresh"

fs_report=$($BIN -c 'koshkit --color never evilfs --all')
case $fs_report in
  *'Source:'*'Volume:'*'UUID:'*'Filesystem ID:'*) fs_detail=matched ;;
  *) fs_detail=wrong ;;
esac
printf 'evilfs-detail=%s\n' "$fs_detail"

net_report=$($BIN -c 'koshkit --color never evilnet')
case $net_report in
  *NAME*FAMILY*ADDRESS*) net_addresses=matched ;;
  *) net_addresses=wrong ;;
esac
printf 'evilnet-addresses=%s\n' "$net_addresses"

net_all=$($BIN -c 'koshkit --color never evilnet --all' 2>/dev/null)
case $net_all in
  *'NAME'*'RX'*'TX'*'Opens:'*'Failures:'*) net_all_sections=matched ;;
  *) net_all_sections=wrong ;;
esac
printf 'evilnet-all-sections=%s\n' "$net_all_sections"

net_failures=$($BIN -c 'koshkit --color never evilnet --failures' 2>/dev/null)
case $net_failures in
  *'Opens:'*'Connections:'*'Failures:'*) net_failure_section=matched ;;
  *) net_failure_section=wrong ;;
esac
printf 'evilnet-failures=%s\n' "$net_failure_section"
