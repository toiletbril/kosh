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

all_report=$(run_report --all)
all_shape=matched
for section in 'cgroup:' 'Membership:' 'Session:' 'Remote sockets:'; do
  case $all_report in
    *"$section"*) ;;
    *) all_shape=missing ;;
  esac
done
printf 'all-shape=%s\n' "$all_shape"

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
