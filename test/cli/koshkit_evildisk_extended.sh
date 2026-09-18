#!/bin/sh

path=$TEST_TEMP_DIRECTORY/evildisk-file
: > "$path"
report=$($BIN -c 'koshkit --color never evildisk --all "$1"' evildisk "$path" \
  2>/dev/null)
case $report in
  *'Status: unavailable'*)
    extended_shape=matched
    ;;
  *) extended_shape=wrong ;;
esac
printf 'extended-shape=%s\n' "$extended_shape"

default_report=$($BIN -c 'koshkit --color never evildisk "$1"' evildisk "$path" \
  2>/dev/null)
case $default_report in
  *IDENTITY*|*"FILESYSTEM FAILURES"*) default_scope=extra ;;
  *) default_scope=capacity ;;
esac
printf 'default-scope=%s\n' "$default_scope"
