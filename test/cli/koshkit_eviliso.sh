#!/bin/sh

run_report()
{
  "$BIN" -c "koshkit --color never eviliso $1"
}

default_report=$(run_report "")
default_shape=matched
for section in 'cgroup:' 'Membership:' 'Session:' 'Remote sockets:'; do
  case $default_report in
    *"$section"*) ;;
    *) default_shape=missing ;;
  esac
done
printf 'default-shape=%s\n' "$default_shape"

detail_report=$(run_report --detail)
detail_shape=matched
case $detail_report in
  *"cgroup:"*"cgroup processes:"*"cgroup process:"*) ;;
  *) detail_shape=missing ;;
esac
case $detail_report in
  *" ("*", "*")"*) ;;
  *) detail_shape=missing ;;
esac
printf 'detail-shape=%s\n' "$detail_shape"

namespaces_only=$(run_report --namespaces)
case $namespaces_only in
  *"Membership:"*) namespaces_scope=wrong ;;
  *) namespaces_scope=matched ;;
esac
printf 'namespaces-scope=%s\n' "$namespaces_scope"

for selector_section in \
  'namespaces|-n|cgroup:' \
  'cgroups|-c|Membership:' \
  'sessions|-s|Session:' \
  'remote|-r|Remote sockets:' \
  'runtime|-k|Runtime:'; do
  old_ifs=$IFS
  IFS='|'
  set -- $selector_section
  IFS=$old_ifs
  report=$(run_report "$2")
  case $report in
    *"$3"*) selector_status=matched ;;
    *) selector_status=missing ;;
  esac
  printf '%s-selector=%s\n' "$1" "$selector_status"
done

help=$($BIN -c 'koshkit eviliso --help')
case $help in
  *"--namespaces"*"--cgroups"*"--sessions"*"--remote"*)
    help_shape=matched
    ;;
  *) help_shape=wrong ;;
esac
printf 'help-shape=%s\n' "$help_shape"
