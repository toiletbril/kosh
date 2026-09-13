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

fs_report=$($BIN -c 'koshkit --color never evilfs --all')
case $fs_report in
  *FILESYSTEMS*'Source:'*'Volume:'*'UUID:'*'OS METADATA'*) fs_detail=matched ;;
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
  *TRAFFIC*TCP*) net_all_sections=matched ;;
  *) net_all_sections=wrong ;;
esac
printf 'evilnet-all-sections=%s\n' "$net_all_sections"

net_failures=$($BIN -c 'koshkit --color never evilnet --failures' 2>/dev/null)
case $net_failures in
  *TCP*Failures:*) net_failure_section=matched ;;
  *) net_failure_section=wrong ;;
esac
printf 'evilnet-failures=%s\n' "$net_failure_section"
