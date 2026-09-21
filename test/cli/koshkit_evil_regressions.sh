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

evilps_sort_prefixes=matched
for evilps_sort_prefix in n p c m; do
  if ! "$BIN" -c \
    "koshkit --color never evilps --sort $evilps_sort_prefix -1" \
    > "$TEST_NULL_DEVICE"
  then
    evilps_sort_prefixes=wrong
  fi
done
printf 'evilps-sort-prefixes=%s\n' "$evilps_sort_prefixes"

evilio_process_sort_keys=matched
for evilio_sort_key in pid read write read-ops write-ops; do
  if ! "$BIN" -c \
    "koshkit --color never evilio --ps --sort $evilio_sort_key -3" \
    > "$TEST_NULL_DEVICE" 2>&1
  then
    evilio_process_sort_keys=wrong
  fi
done
printf 'evilio-process-sort-keys=%s\n' "$evilio_process_sort_keys"

evilio_disk_sort_keys=matched
for evilio_sort_key in read write read-ops write-ops busy read-latency \
  write-latency average-queue queue errors retries
do
  if ! "$BIN" -c \
    "koshkit --color never evilio --all --sort $evilio_sort_key" \
    > "$TEST_NULL_DEVICE" 2>&1
  then
    evilio_disk_sort_keys=wrong
  fi
done
printf 'evilio-disk-sort-keys=%s\n' "$evilio_disk_sort_keys"

evilio_ambiguous_sort=$(
  "$BIN" -c 'koshkit evilio --ps --sort r -1' 2>&1
)
case $evilio_ambiguous_sort in
  *'Ambiguous sort key'*) evilio_ambiguous_sort_status=matched ;;
  *) evilio_ambiguous_sort_status=wrong ;;
esac
printf 'evilio-ambiguous-sort=%s\n' "$evilio_ambiguous_sort_status"

evilio_unavailable_sort=$(
  "$BIN" -c 'koshkit evilio --ps --sort busy -1' 2>&1
)
case $evilio_unavailable_sort in
  *'unavailable for process reports'*) evilio_unavailable_sort_status=matched ;;
  *) evilio_unavailable_sort_status=wrong ;;
esac
printf 'evilio-unavailable-sort=%s\n' "$evilio_unavailable_sort_status"

evilio_live_sort_path=$TEST_TEMP_DIRECTORY/evilio-live-sort-report
set -m
"$BIN" -c \
  'koshkit --color never evilio --ps --sort read --live=0.05 --cumulative=0.1' \
  > "$evilio_live_sort_path" &
evilio_live_sort_pid=$!
set +m
sleep 0.30
if kill -0 "$evilio_live_sort_pid" 2> "$TEST_NULL_DEVICE"; then
  kill -INT "$evilio_live_sort_pid"
fi
wait "$evilio_live_sort_pid"
printf 'evilio-live-sort-status=%s\n' "$?"
evilio_live_sort_report=$(< "$evilio_live_sort_path")
case $evilio_live_sort_report in
  *'READ/0.1s'*'ctrl+c to exit'*) evilio_live_sort_update=matched ;;
  *) evilio_live_sort_update=wrong ;;
esac
printf 'evilio-live-sort=%s\n' "$evilio_live_sort_update"

evilio_help=$($BIN -c 'koshkit evilio --help')
if printf '%s\n' "$evilio_help" | grep -Fq 'LIVE OPTIONS' &&
  printf '%s\n' "$evilio_help" | grep -Fq -- '--live[=<seconds>]' &&
  printf '%s\n' "$evilio_help" | grep -Fq -- '--cumulative[=<seconds>]' &&
  printf '%s\n' "$evilio_help" | grep -Fq -- '--sort=<...>'
then
  evilio_help_controls=matched
else
  evilio_help_controls=wrong
fi
printf 'evilio-help-controls=%s\n' "$evilio_help_controls"

evilnet_help=$($BIN -c 'koshkit evilnet --help')
if printf '%s\n' "$evilnet_help" | grep -Fq 'LIVE OPTIONS' &&
  printf '%s\n' "$evilnet_help" | grep -Fq -- '--live[=<seconds>]' &&
  printf '%s\n' "$evilnet_help" | grep -Fq -- '--cumulative[=<seconds>]'
then
  evilnet_help_controls=matched
else
  evilnet_help_controls=wrong
fi
printf 'evilnet-help-controls=%s\n' "$evilnet_help_controls"

evil_completion=$(< ../completions/kosh.bash)
case $evil_completion in
  *'evilio)'*'-C --cumulative -l --live'*'--sort'*)
    evilio_completion_controls=matched ;;
  *) evilio_completion_controls=wrong ;;
esac
case $evil_completion in
  *'evilnet)'*'-l --live -C --cumulative'*)
    evilnet_completion_controls=matched ;;
  *) evilnet_completion_controls=wrong ;;
esac
case $evil_completion in
  *'evilps)'*'-w --wide --sort -l --live -C --cumulative'*)
    evilps_completion_controls=matched ;;
  *) evilps_completion_controls=wrong ;;
esac
printf 'evilio-completion-controls=%s\n' "$evilio_completion_controls"
printf 'evilnet-completion-controls=%s\n' "$evilnet_completion_controls"
printf 'evilps-completion-controls=%s\n' "$evilps_completion_controls"

evilnet_sort_keys=matched
for evilnet_sort_key in name rx tx rx-packets tx-packets rx-errors \
  tx-errors rx-drops tx-drops
do
  if ! "$BIN" -c \
    "koshkit --color never evilnet --traffic --sort $evilnet_sort_key" \
    > "$TEST_NULL_DEVICE" 2>&1
  then
    evilnet_sort_keys=wrong
  fi
done
printf 'evilnet-sort-keys=%s\n' "$evilnet_sort_keys"

evilnet_ambiguous_sort=$(
  "$BIN" -c 'koshkit evilnet --traffic --sort r' 2>&1
)
case $evilnet_ambiguous_sort in
  *'ambiguous sort key'*) evilnet_ambiguous_sort_status=matched ;;
  *) evilnet_ambiguous_sort_status=wrong ;;
esac
printf 'evilnet-ambiguous-sort=%s\n' "$evilnet_ambiguous_sort_status"

evilnet_live_sort_path=$TEST_TEMP_DIRECTORY/evilnet-live-sort-report
set -m
"$BIN" -c \
  'koshkit --color never evilnet --traffic --sort tx --live=0.05 --cumulative=0.1' \
  > "$evilnet_live_sort_path" &
evilnet_live_sort_pid=$!
set +m
sleep 0.30
if kill -0 "$evilnet_live_sort_pid" 2> "$TEST_NULL_DEVICE"; then
  kill -INT "$evilnet_live_sort_pid"
fi
wait "$evilnet_live_sort_pid"
printf 'evilnet-live-sort-status=%s\n' "$?"
evilnet_live_sort_report=$(< "$evilnet_live_sort_path")
case $evilnet_live_sort_report in
  *'TX/0.1s'*'ctrl+c to exit'*) evilnet_live_sort_update=matched ;;
  *) evilnet_live_sort_update=wrong ;;
esac
printf 'evilnet-live-sort=%s\n' "$evilnet_live_sort_update"

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
$BIN -c 'koshkit evilps --live=bad' > /dev/null 2>&1
printf 'evilps-rejected-live=%s\n' "$?"
$BIN -c 'koshkit evilps --cumulative=bad' > /dev/null 2>&1
printf 'evilps-rejected-cumulative=%s\n' "$?"

"$TEST_SHELL" -c 'while :; do :; done' &
busy_process_pid=$!
evilps_ordinary_cpu=$(
  "$BIN" -c "koshkit --color never evilps --cpu $busy_process_pid"
)
case $evilps_ordinary_cpu in
  *'CPU '*'s]'*) evilps_ordinary_cpu_unit=matched ;;
  *) evilps_ordinary_cpu_unit=wrong ;;
esac
printf 'evilps-ordinary-cpu-unit=%s\n' "$evilps_ordinary_cpu_unit"
evilps_sampled_cpu=$(
  "$BIN" -c \
    "koshkit --color never evilps --cpu --cumulative=0.3 $busy_process_pid"
)
kill -TERM "$busy_process_pid"
wait "$busy_process_pid" 2> "$TEST_NULL_DEVICE"
case $evilps_sampled_cpu in
  *'CPU 0.00%'*|*'CPU -'*) evilps_sampled_cpu_unit=wrong ;;
  *'CPU '*'%'*) evilps_sampled_cpu_unit=matched ;;
  *) evilps_sampled_cpu_unit=wrong ;;
esac
printf 'evilps-sampled-cpu-unit=%s\n' "$evilps_sampled_cpu_unit"

evilps_live_path=$TEST_TEMP_DIRECTORY/evilps-live-report
set -m
"$BIN" -c 'koshkit --color never evilps --cpu --show-pids --live=0.05 --cumulative=0.1 -1' \
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
evilps_live_report=$(< "$evilps_live_path")
case $evilps_live_report in
  *'ctrl+c to exit. cumulative stats over 0.1s every 0.05s'*)
    evilps_live_controls=matched
    ;;
  *) evilps_live_controls=wrong ;;
esac
case $evilps_live_report in
  *'CPU -'*) evilps_live_initial=matched ;;
  *) evilps_live_initial=wrong ;;
esac
case $evilps_live_report in
  *'CPU '*'%'*) evilps_live_percentage=matched ;;
  *) evilps_live_percentage=wrong ;;
esac
if [ "$evilps_live_controls" = matched ] &&
  [ "$evilps_live_initial" = matched ] &&
  [ "$evilps_live_percentage" = matched ]
then
  evilps_live_window=matched
else
  evilps_live_window=wrong
fi
printf 'evilps-live-window=%s\n' "$evilps_live_window"

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
