#!/bin/sh

report=$($BIN -c 'koshkit --color never evilnet --failures' 2>/dev/null)
case $report in
  *'Opens:'*'Connections:'*'Failures:'*) failure_shape=matched ;;
  *) failure_shape=wrong ;;
esac
printf 'failure-shape=%s\n' "$failure_shape"
case $report in
  *'NAME'*'RX'*'TX'*) scope_shape=extra ;;
  *) scope_shape=isolated ;;
esac
printf 'failure-scope=%s\n' "$scope_shape"

conflict_status=0
$BIN -c 'koshkit evilnet --failures --live' >/dev/null 2>&1 || conflict_status=$?
printf 'live-conflict-status=%s\n' "$conflict_status"

help=$($BIN -c 'koshkit evilnet --help')
case $help in
  *"--failures"*) help_shape=matched ;;
  *) help_shape=wrong ;;
esac
printf 'help-shape=%s\n' "$help_shape"
