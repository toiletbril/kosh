#!/bin/sh

root=$TEST_TEMP_DIRECTORY/goodfsw-trx
event_output=$TEST_TEMP_DIRECTORY/goodfsw-trx.out
mkdir -p "$root/sub"
: > "$root/sub/file"
: > "$event_output"

(
  while [ ! -s "$event_output" ]; do
    printf x >> "$root/sub/file"
    sleep 0.02
  done
) &
writer_pid=$!

"$BIN" -c 'koshkit goodfsw -trx1 -l 0.02 "$1"' goodfsw "$root" \
  > "$event_output" 2>&1
watcher_status=$?
wait "$writer_pid"
IFS=' ' read -r event_time event_path event_name event_kind < "$event_output"
case $event_time in
  ''|*[!0-9]*) event_time=nonnumeric ;;
  *) event_time=numeric ;;
esac

printf 'status=%s\n' "$watcher_status"
printf 'timestamp=%s\n' "$event_time"
printf 'path=%s\n' "$event_path"
printf 'event=%s\n' "$event_name"
printf 'kind=%s\n' "$event_kind"

node_root=$TEST_TEMP_DIRECTORY/goodnode-batch
node_path=$node_root/a/b/needle
mkdir -p "$node_root/a/b"
: > "$node_path"
inode=$("$BIN" -c 'koshkit stat -c %i "$1"' stat "$node_path")
node_report=$("$BIN" -c 'koshkit --color never goodnode -r "$1" -i "$2"' \
  goodnode "$node_root" "$inode")
node_status=$?
case $node_report in
  "$node_path"*) node_path_status=matched ;;
  *) node_path_status=wrong ;;
esac
printf 'goodnode-status=%s\n' "$node_status"
printf 'goodnode-path=%s\n' "$node_path_status"

default_report=$("$BIN" -c 'koshkit --color never evilio')
case $default_report in
  MEMORY*DISKS*SWAP*) default_shape=matched ;;
  *) default_shape=wrong ;;
esac
case $default_report in
  *CPU*|*PAGING*|*SCHEDULER*|*STALLS*|*Activity*) default_scope=extra ;;
  *) default_scope=instant ;;
esac
printf 'evilio-default-shape=%s\n' "$default_shape"
printf 'evilio-default-scope=%s\n' "$default_scope"

disk_report=$("$BIN" -c 'koshkit --color never evilio --cumulative 0.05')
case $disk_report in
  DEVICE*READ/S*WRITE/S*"READ OPS/S"*"WRITE OPS/S"*BUSY*"READ LAT"*"WRITE LAT"*"AVG QUEUE"*QUEUE*ERRORS*RETRIES*)
    disk_shape=matched
    ;;
  *) disk_shape=wrong ;;
esac
case $disk_report in
  *CPU*|*MEMORY*|*PAGING*|*SCHEDULER*|*STALLS*|*SWAP*) disk_scope=extra ;;
  *) disk_scope=only-io ;;
esac
printf 'evilio-disk-shape=%s\n' "$disk_shape"
printf 'evilio-disk-scope=%s\n' "$disk_scope"
disk_first_row=$(printf '%s\n' "$disk_report" | sed -n '2p')
case $disk_first_row in
  '') disk_row_layout=missing ;;
  '  '*) disk_row_layout=indented ;;
  *) disk_row_layout=unindented ;;
esac
printf 'evilio-disk-row=%s\n' "$disk_row_layout"

evilio_help=$("$BIN" -c 'koshkit evilio --help')
case $evilio_help in
  *"--cumulative[=<seconds>]"*) cumulative_help=named-seconds ;;
  *) cumulative_help=unclear ;;
esac
printf 'evilio-cumulative-help=%s\n' "$cumulative_help"

process_report=$("$BIN" -c \
  'koshkit --color never evilio --cumulative=0.05 --ps --count 1')
case $process_report in
  *PID*READ/S*WRITE/S*"READ IOPS"*"WRITE IOPS"*COMMAND*)
    process_shape=matched
    ;;
  *) process_shape=wrong ;;
esac
case $process_report in
  *DISKS*|*MEMORY*|*PAGING*|*SCHEDULER*|*STALLS*|*SWAP*)
    process_scope=extra
    ;;
  *) process_scope=only-io ;;
esac
printf 'evilio-process-shape=%s\n' "$process_shape"
printf 'evilio-process-scope=%s\n' "$process_scope"

process_report_path=$TEST_TEMP_DIRECTORY/evilio-process-report
printf '%s\n' "$process_report" > "$process_report_path"
process_idle=excluded
while read -r process_pid process_read process_write process_read_iops \
  process_write_iops process_command; do
  case "$process_pid:$process_read:$process_write:$process_read_iops:$process_write_iops" in
    [0-9]*:0:0:0:0) process_idle=present ;;
    [0-9]*:0:0:-:-) process_idle=present ;;
  esac
done < "$process_report_path"
printf 'evilio-process-idle=%s\n' "$process_idle"

"$BIN" -c \
  'koshkit --color never evilio --cumulative --ps -1 0.05' \
  > "$TEST_NULL_DEVICE"
printf 'evilio-process-duration-limit=%s\n' "$?"

all_process_report=$(
  "$BIN" -c 'koshkit --color never evilio --ps'
)
limited_process_report=$(
  "$BIN" -c 'koshkit --color never evilio --ps -1'
)
all_process_line_count=$(printf '%s\n' "$all_process_report" | wc -l)
limited_process_line_count=$(printf '%s\n' "$limited_process_report" | wc -l)
if [ "$all_process_line_count" -gt "$limited_process_line_count" ]; then
  process_limit=unlimited
else
  process_limit=wrong
fi
printf 'evilio-process-limit=%s\n' "$process_limit"

network_report=$("$BIN" -c 'koshkit evilnet')
case $network_report in
  NAME*FAMILY*ADDRESS*) network_title=omitted ;;
  *) network_title=present ;;
esac
printf 'evilnet-single-title=%s\n' "$network_title"

network_all_report=$("$BIN" -c 'koshkit evilnet --all' \
  2> "$TEST_NULL_DEVICE")
case $network_all_report in
  INTERFACES*) network_all_title=present ;;
  *) network_all_title=missing ;;
esac
printf 'evilnet-multiple-titles=%s\n' "$network_all_title"

network_traffic_report=$("$BIN" -c 'koshkit evilnet --traffic' 2> "$TEST_NULL_DEVICE")
case $network_traffic_report in
  TRAFFIC*) network_traffic_only=present ;;
  *) network_traffic_only=missing ;;
esac
printf 'evilnet-traffic-only=%s\n' "$network_traffic_only"

process_tree=$("$BIN" -c 'koshkit evilps -1')
case $process_tree in
  PROCESSES*) process_title=present ;;
  *) process_title=omitted ;;
esac
printf 'evilps-single-title=%s\n' "$process_title"

filesystem_report=$("$BIN" -c 'koshkit evilfs')
case $filesystem_report in
  SOURCE*TARGET*TYPE*OPTIONS*) filesystem_title=omitted ;;
  *) filesystem_title=present ;;
esac
printf 'evilfs-single-title=%s\n' "$filesystem_title"

filesystem_all_report=$("$BIN" -c 'koshkit evilfs --all')
case $filesystem_all_report in
  FILESYSTEMS*) filesystem_all_title=present ;;
  *) filesystem_all_title=missing ;;
esac
printf 'evilfs-multiple-titles=%s\n' "$filesystem_all_title"

cores_report=$("$BIN" -c 'koshkit evillogs --cores')
case $cores_report in
  CORES*|*LOGS*) cores_title=present ;;
  *) cores_title=omitted ;;
esac
printf 'evillogs-single-title=%s\n' "$cores_title"

logs_report=$("$BIN" -c 'koshkit evillogs --cores --logs')
case $logs_report in
  CORES*LOGS*) logs_titles=present ;;
  *) logs_titles=missing ;;
esac
printf 'evillogs-multiple-titles=%s\n' "$logs_titles"

host_system=$("$BIN" -c 'koshkit uname -s')
evildisk_tools=$TEST_TEMP_DIRECTORY/evildisk-tools
mkdir -p "$evildisk_tools"
if [ "$host_system" = Darwin ]; then
  printf '%s\n' '#!/bin/sh' \
    'printf "%s\n" "SMART data unavailable"' > "$evildisk_tools/smartctl"
  printf '%s\n' '#!/bin/sh' \
    'printf "%s\n" "Device Identifier: disk-test" "SMART Status: Verified" "Device / Media Name: Mock Disk" "Protocol: NVMe" "Temperature: 42 Celsius" "Percentage Used: 7%" "Power On Hours: 123" "Unsafe Shutdowns: 2" "Media and Data Integrity Errors: 5,075" "Data Units Read: 1,000" "Data Units Written: 2,000"' \
    > "$evildisk_tools/diskutil"
  chmod 755 "$evildisk_tools/smartctl" "$evildisk_tools/diskutil"
  evildisk_report=$(PATH="$evildisk_tools:$PATH" "$BIN" -c \
    'koshkit --color never evildisk -a /dev/null' 2>&1)
  case $evildisk_report in
    *disk-test*Verified*temperature*"42 Celsius"*"used 7%"*"media errors 5,075"*warning:*"nonzero SMART counters"*)
      evildisk_fallback=passed
      ;;
    *) evildisk_fallback=failed ;;
  esac
else
  evildisk_fallback=passed
fi
printf '%s\n' '#!/bin/sh' \
  'printf "%s\n" "Device Model: Mock ATA" "Transport protocol: SATA" "SMART overall-health self-assessment test result: PASSED" "ID# ATTRIBUTE_NAME FLAG VALUE WORST THRESH TYPE UPDATED WHEN_FAILED RAW_VALUE" "5 Retired_Block_Count 0x0033 100 100 010 Pre-fail Always - 2" "188 Command_Timeouts 0x0032 100 100 000 Old_age Always - 0" "197 Pending_Sectors 0x0012 100 100 000 Old_age Always - 3" "198 Truncated_Row 0x0010" "199 CRC_Error_Count 0x003e 200 200 000 Old_age Always - 4"' \
  > "$evildisk_tools/smartctl"
chmod 755 "$evildisk_tools/smartctl"
evildisk_ata_report=$(PATH="$evildisk_tools:$PATH" "$BIN" -c \
  'koshkit --color never evildisk -a /dev/null' 2>&1)
case $evildisk_ata_report in
  *uncorrectable*) evildisk_ata=failed ;;
  *"Mock ATA"*"reallocated 2"*"timeouts 0"*"pending 3"*"CRC errors 4"*warning:*"nonzero SMART counters reallocated 2, pending 3, CRC errors 4"*)
    evildisk_ata=passed
    ;;
  *) evildisk_ata=failed ;;
esac
printf 'evildisk-smart-fallback=%s\n' "$evildisk_fallback"
printf 'evildisk-smart-ata=%s\n' "$evildisk_ata"

"$BIN" -c 'koshkit --color never evilio --all --cumulative' \
  > "$TEST_NULL_DEVICE" 2>&1
printf 'evilio-conflict=%s\n' "$?"
