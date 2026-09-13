#!/bin/sh

root=$TEST_TEMP_DIRECTORY/goodfsw-format
mkdir -p "$root"

run_watch() {
  watch_output=$1
  watch_flags=$2
  "$BIN" -c "koshkit goodfsw $watch_flags -l 0.02 -1 '$root'" \
    > "$watch_output" &
  watch_pid=$!
  watch_attempt=0
  while [ "$watch_attempt" -lt 100 ] && kill -0 "$watch_pid" 2> "$TEST_NULL_DEVICE"; do
    : > "$root/file"
    sleep 0.02
    watch_attempt=$((watch_attempt + 1))
  done
  wait "$watch_pid"
  watch_status=$?
  watch_line=$(cat "$watch_output")
}

run_watch "$TEST_TEMP_DIRECTORY/goodfsw-machine" ""
machine_status=$watch_status
machine_line=$watch_line
case $machine_line in
  [0-9]*\ [0-9]*\ *"$root"/*) machine_shape=matched ;;
  *) machine_shape=wrong ;;
esac

run_watch "$TEST_TEMP_DIRECTORY/goodfsw-human" "--human-readable"
human_status=$watch_status
human_line=$watch_line
case $human_line in
  ????-??-??\ ??*) human_timestamp=matched ;;
  *) human_timestamp=wrong ;;
esac
case $human_line in
  *"$root"/*\ Created\ *|*"$root"/*\ Updated\ *|\
    *"$root"/*\ Removed\ *|*"$root"/*\ AttributeModified\ *)
    human_event=matched
    ;;
  *) human_event=wrong ;;
esac
if [ "$human_timestamp" = matched ] && [ "$human_event" = matched ]; then
  human_shape=matched
else
  human_shape=wrong
fi

printf 'machine-status=%s\n' "$machine_status"
printf 'machine-shape=%s\n' "$machine_shape"
printf 'human-status=%s\n' "$human_status"
printf 'human-shape=%s\n' "$human_shape"
