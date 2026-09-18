#!/bin/sh

report=$($BIN -c 'koshkit --color never evilfiles --pid $$' 2> "$TEST_NULL_DEVICE")
case $report in
  *MODE*OFFSET*ENDPOINT*NAME*) schema=present ;;
  *) schema=missing ;;
esac
printf 'schema=%s\n' "$schema"

invalid_status=0
$BIN -c 'koshkit evilfiles --pid invalid' > "$TEST_NULL_DEVICE" 2>&1 || invalid_status=$?
printf 'invalid-pid-status=%s\n' "$invalid_status"

case $report in
  *'(deleted)'*|*'[inaccessible]'*|*socket:*|*pipe*)
    descriptor_markers=present
    ;;
  *) descriptor_markers=capability-dependent
    ;;
esac
printf 'descriptor-markers=%s\n' "$descriptor_markers"
