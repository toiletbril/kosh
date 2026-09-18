#!/bin/sh

run_report()
{
  "$BIN" -c "koshkit --color never eviliso $1"
}

default_report=$(run_report "")
default_shape=matched
for section in 'cgroup:' 'Membership:' 'Session:' 'Remote sockets:'; do
  :
done
case $default_report in
  *"cgroup:"*"Controller 0:"*"Session:"*"Remote sockets:"*)
    default_shape=matched
    ;;
  *) default_shape=missing ;;
esac
printf 'default-shape=%s\n' "$default_shape"

all_report=$(run_report -a)
detail_shape=matched
case $all_report in
  *"cgroup:"*"cgroup processes:"*"cgroup process:"*) ;;
  *) detail_shape=missing ;;
esac
case $all_report in
  *" ("*", "*")"*) ;;
  *) detail_shape=missing ;;
esac
case $all_report in
  *" ("*", self; "*" namespace "*")"*) ;;
  *) detail_shape=missing ;;
esac
case $all_report in
  *"Controller 0:"*"Login time:"*"Remote peer 0:"*) ;;
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
  'cgroups|-c|Controller 0:' \
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

"$BIN" -c 'koshkit evilps >/dev/null; koshkit evilss -x >/dev/null; koshkit eviliso -n >/dev/null'
echo "unprivileged-status=$?"
