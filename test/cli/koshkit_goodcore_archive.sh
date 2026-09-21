#!/bin/sh

work=$TEST_TEMP_DIRECTORY/goodcore-archive
mkdir -p "$work"
core=$work/fake.core
uncompressed=$work/archive.tar
compressed_work=$work/compressed
mkdir -p "$compressed_work"
cp "$BIN" "$core"

report=$($BIN -c 'koshkit --color never goodcore --binary "$1" --output "$2" --no-compress "$3"' \
  goodcore "$BIN" "$uncompressed" "$core")
status=$?
printf 'uncompressed-status=%s\n' "$status"
case $report in
  GOODCORE*|*"  Archive:"*) report_shape=wrong ;;
  Archive:*Executable:*Files:*) report_shape=matched ;;
  *) report_shape=wrong ;;
esac
printf 'report-shape=%s\n' "$report_shape"

contents=$(tar -tf "$uncompressed")
archive_shape=matched
for required_entry in './dump/core' './INFO.txt' './root/'; do
  case $contents in
    *"$required_entry"*) ;;
    *) archive_shape=wrong ;;
  esac
done
printf 'uncompressed-contents=%s\n' "$archive_shape"

quiet_status=0
$BIN -c 'koshkit goodcore --quiet --binary "$1" --output "$2" --no-compress "$3"' \
  goodcore "$BIN" "$work/quiet.tar" "$core" >/dev/null 2>&1 || quiet_status=$?
printf 'quiet-status=%s\n' "$quiet_status"

compressed_status=0
if command -v zstd >/dev/null 2>&1; then
  compressed_suffix=.tar.zst
else
  compressed_suffix=.tar.gz
fi
compressed_target=$compressed_work/archive$compressed_suffix
"$BIN" -c \
  'koshkit goodcore --quiet --binary "$1" --output "$2" "$3"' \
  goodcore "$BIN" "$compressed_target" "$core" \
  >/dev/null 2>&1 || compressed_status=$?
printf 'compressed-status=%s\n' "$compressed_status"
if [ -f "$compressed_target" ]; then
  compressed_shape=matched
else
  compressed_shape=wrong
fi
printf 'compressed-suffix=%s\n' "$compressed_suffix"
printf 'compressed-shape=%s\n' "$compressed_shape"
