#!/bin/sh

report=$($BIN -c 'koshkit --color never evilfs --all')

case $report in
  *'Source:'*'Options:'*) detail_shape=matched ;;
  *) detail_shape=missing ;;
esac
printf 'detail-shape=%s\n' "$detail_shape"

case $report in
  *'Warning: skipped '* )
    warning_shape=capitalized
    ;;
  *)
    warning_shape=none
    ;;
esac
printf 'permission-warning=%s\n' "$warning_shape"
