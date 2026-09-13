#!/bin/sh

fixture=$TEST_TEMP_DIRECTORY/goodstat-extended
printf 'goodstat fixture\n' > "$fixture"

default_report=$($BIN -c 'koshkit --color never goodstat "$1"' goodstat "$fixture")
case $default_report in
  *Filesystem:*|*CRC32C:*) default_scope=extra ;;
  *) default_scope=portable ;;
esac
printf 'default-scope=%s\n' "$default_scope"

filesystem_report=$($BIN -c 'koshkit --color never goodstat --filesystem "$1"' \
  goodstat "$fixture")
case $filesystem_report in
  *Filesystem:*'Filesystem block size:'*'Filesystem capacity:'*)
    filesystem_shape=matched
    ;;
  *) filesystem_shape=wrong ;;
esac
printf 'filesystem-shape=%s\n' "$filesystem_shape"

checksum_report=$($BIN -c 'koshkit --color never goodstat --checksum "$1"' \
  goodstat "$fixture")
checksum_line=$(printf '%s\n' "$checksum_report" | sed -n '/CRC32C:/p')
case $checksum_line in
  '  CRC32C: '????????) checksum_shape=matched ;;
  *) checksum_shape=wrong ;;
esac
printf 'checksum-shape=%s\n' "$checksum_shape"

combined_status=0
$BIN -c 'koshkit --color never goodstat -f -c "$1"' goodstat "$fixture" \
  >/dev/null 2>&1 || combined_status=$?
printf 'combined-status=%s\n' "$combined_status"

help=$($BIN -c 'koshkit goodstat --help')
case $help in
  *"--checksum"*"--filesystem"*) help_shape=matched ;;
  *) help_shape=wrong ;;
esac
printf 'help-shape=%s\n' "$help_shape"
