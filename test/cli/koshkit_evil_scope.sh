#!/bin/sh

default_report=$($BIN -c 'koshkit --color never evil')
case $default_report in
  *'Users:'*) default_users=expanded ;;
  *) default_users=momentary ;;
esac
printf 'default-users=%s\n' "$default_users"
case $default_report in
  *PROCFS*|*ANOMALIES*) default_extended=extra ;;
  *) default_extended=omitted ;;
esac
printf 'default-extended=%s\n' "$default_extended"

users_report=$($BIN -c 'koshkit --color never evil --users')
case $users_report in
  *'Users:'*) users_selector=matched ;;
  *) users_selector=missing ;;
esac
printf 'users-selector=%s\n' "$users_selector"

all_report=$($BIN -c 'koshkit --color never evil --all')
case $all_report in
  *'Page faults:'*'Mixed libraries:'*) all_extended=matched ;;
  *) all_extended=missing ;;
esac
printf 'all-extended=%s\n' "$all_extended"

short_report=$($BIN -c 'koshkit --color never evil --short')
case $short_report in
  *Uptime*|*PROCFS*|*ANOMALIES*) short_scope=extra ;;
  *) short_scope=identity ;;
esac
printf 'short-scope=%s\n' "$short_scope"
