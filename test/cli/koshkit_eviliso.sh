#!/bin/sh

run_report()
{
  "$BIN" -c "koshkit --color never eviliso $1"
}

cgroup_work=$TEST_MKTEMP_DIRECTORY/eviliso-cgroup-$$
cgroup_exit_pid=
cgroup_report_pid=
cgroup_writer_pid=
has_cgroup_writer_descriptor=no
has_cgroup_exit_descriptor=no
cleanup()
{
  for child_pid in "$cgroup_report_pid" "$cgroup_writer_pid" \
      "$cgroup_exit_pid"; do
    if test -n "$child_pid"; then
      kill "$child_pid" 2>/dev/null
      wait "$child_pid" 2>/dev/null
    fi
  done
  if test "$has_cgroup_writer_descriptor" = yes; then
    exec 6>&-
  fi
  if test "$has_cgroup_exit_descriptor" = yes; then
    exec 7>&-
  fi
  if test -n "$cgroup_work" && test -d "$cgroup_work"; then
    "$TEST_SYSTEM_RM" -rf -- "$cgroup_work"
  fi
}
trap cleanup EXIT

has_cgroup_section()
{
  case $1 in
  *"HIERARCHY"*"CONTROLLER"*|*"Membership: unavailable"*) return 0 ;;
  *) return 1 ;;
  esac
}

default_report=$(run_report "")
default_shape=matched
case $default_report in
  *"cgroup:"*"Count:"*"Remote sockets:"*"Runtime:"*)
    default_shape=matched
    ;;
  *) default_shape=missing ;;
esac
has_cgroup_section "$default_report" || default_shape=missing
printf 'default-shape=%s\n' "$default_shape"

cgroup_detail_report=$(run_report '-a -c')
detail_shape=matched
case $cgroup_detail_report in
  *"HIERARCHY"*"CONTROLLER"*"PATH"*"PID"*"NAME"*"ROLE"*"self"*) ;;
  *"Membership: unavailable"*) ;;
  *) detail_shape=missing ;;
esac
printf 'detail-shape=%s\n' "$detail_shape"

remote_report=$(run_report --remote)
case $remote_report in
  *"Remote sockets:"*"Total sockets:"*"FAMILY"*"PROTO"*"STATE"*"RECV-Q"*\
*"SEND-Q"*"LOCAL"*"PEER"*"SOCKET"*"PID"*"UID"*"USER"*"NAME"*\
*"COMMAND"*"NETNS"*"ORCHESTRATOR"*"RUNTIME"*"CONTAINER"*"CGROUP"*)
    remote_table=matched
    ;;
  *) remote_table=missing ;;
esac
printf 'remote-table=%s\n' "$remote_table"

for selector_section in \
  'namespaces|-n|cgroup:' \
  'cgroups|-c|cgroup-section' \
  'sessions|-s|Count:' \
  'remote|-r|Remote sockets:' \
  'runtime|-k|Runtime:'; do
  old_ifs=$IFS
  IFS='|'
  set -- $selector_section
  IFS=$old_ifs
  report=$(run_report "$2")
  if test "$1" = cgroups; then
    if has_cgroup_section "$report"; then
      selector_status=matched
    else
      selector_status=missing
    fi
  else
    case $report in
      *"$3"*) selector_status=matched ;;
      *) selector_status=missing ;;
    esac
  fi
  printf '%s-selector=%s\n' "$1" "$selector_status"

  selector_scope=matched
  case $1 in
  namespaces)
    case $report in
    *"HIERARCHY"*|*"Count:"*|*"Remote sockets:"*|*"Runtime:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  cgroups)
    case $report in
    *"cgroup:"*|*"Count:"*|*"Remote sockets:"*|*"Runtime:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  sessions)
    case $report in
    *"cgroup:"*|*"HIERARCHY"*|*"Remote sockets:"*|*"Runtime:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  remote)
    case $report in
    *"cgroup:"*|*"HIERARCHY"*|*"Count:"*|*"Runtime:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  runtime)
    case $report in
    *"cgroup:"*|*"HIERARCHY"*|*"Count:"*|*"Remote sockets:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  esac
  printf '%s-scope=%s\n' "$1" "$selector_scope"
done

combined_report=$(run_report '-n -k')
case $combined_report in
*"cgroup:"*"Runtime:"*) combined_scope=matched ;;
*) combined_scope=missing ;;
esac
case $combined_report in
*"HIERARCHY"*|*"Count:"*|*"Remote sockets:"*) combined_scope=wrong ;;
esac
printf 'combined-scope=%s\n' "$combined_scope"

namespace_detail=$(run_report '-a -n')
case $namespace_detail in
*"cgroup process:"*) all_scope=matched ;;
*) all_scope=missing ;;
esac
case $namespace_detail in
*"HIERARCHY"*|*"Count:"*|*"Remote sockets:"*|*"Runtime:"*) all_scope=wrong ;;
esac
printf 'all-scope=%s\n' "$all_scope"

all_report=$(run_report -a)
case $all_report in
*"cgroup:"*"HIERARCHY"*"Count:"*"Remote sockets:"*"Runtime:"*)
  all_default_scope=matched
  ;;
*"cgroup:"*"Membership: unavailable"*"Count:"*"Remote sockets:"*"Runtime:"*)
  all_default_scope=matched
  ;;
*) all_default_scope=missing ;;
esac
printf 'all-default-scope=%s\n' "$all_default_scope"

mkdir -p "$cgroup_work/self" "$cgroup_work/$$" || exit 1
printf '%s\n' \
  '41:kosh-test-exact:/target' \
  '0::/unified-target' \
  '41:kosh-test-exact:/target' \
  > "$cgroup_work/self/cgroup"
printf '%s\n' \
  '41:kosh-test-exact:/target' \
  '0::/unified-target' \
  '42:kosh-test-exact:/target' \
  '41:kosh-test-other:/target' \
  '41:kosh-test-exact:/other' \
  '0::/other' \
  > "$cgroup_work/$$/cgroup"
cgroup_synthetic=matched
if test "${IS_NONDEBUG_BUILD:-0}" = 0; then
  synthetic_report=$cgroup_work/debug-report
  KOSH_TEST_CGROUP_PROC=$cgroup_work \
    "$BIN" -c 'koshkit --color never eviliso -a -c' \
    > "$synthetic_report" || cgroup_synthetic=wrong
  self_count=0
  other_count=0
  has_v1=no
  has_v2=no
  while IFS= read -r row; do
    set -- $row
    test "$#" -eq 6 || continue
    test "$1" = HIERARCHY && continue
    case "$1:$2:$3" in
    41:kosh-test-exact:/target) has_v1=yes ;;
    0:unified:/unified-target) has_v2=yes ;;
    *) cgroup_synthetic=wrong ;;
    esac
    case $6 in
    self) self_count=$((self_count + 1)) ;;
    other)
      test "$4" = "$$" || cgroup_synthetic=wrong
      other_count=$((other_count + 1))
      ;;
    *) cgroup_synthetic=wrong ;;
    esac
  done < "$synthetic_report"
  test "$self_count" -eq 2 && test "$other_count" -eq 2 && \
    test "$has_v1" = yes && test "$has_v2" = yes || cgroup_synthetic=wrong

  race_root=$cgroup_work/race
  exit_release=$cgroup_work/exit-release
  writer_release=$cgroup_work/writer-release
  writer_ready=$cgroup_work/writer-ready
  mkdir -p "$race_root/self" || exit 1
  mkfifo "$exit_release" "$writer_release" || exit 1
  exec 6<> "$writer_release" || exit 1
  has_cgroup_writer_descriptor=yes
  exec 7<> "$exit_release" || exit 1
  has_cgroup_exit_descriptor=yes
  (IFS= read -r release_byte < "$exit_release") &
  cgroup_exit_pid=$!
  mkdir -p "$race_root/$cgroup_exit_pid" || exit 1
  mkfifo "$race_root/$cgroup_exit_pid/cgroup" || exit 1
  printf '%s\n' '51:kosh-test-race:/target' > "$race_root/self/cgroup"
  race_report=$cgroup_work/race-report
  KOSH_TEST_CGROUP_PROC=$race_root \
    "$BIN" -c 'koshkit --color never eviliso -a -c' \
    > "$race_report" &
  cgroup_report_pid=$!
  (
    exec 5> "$race_root/$cgroup_exit_pid/cgroup" || exit 1
    : > "$writer_ready"
    IFS= read -r release_byte <&6 || exit 1
    printf '%s\n' '51:kosh-test-race:/target' >&5
  ) &
  cgroup_writer_pid=$!
  writer_wait_count=0
  while test ! -e "$writer_ready" && \
      kill -0 "$cgroup_report_pid" 2>/dev/null && \
      kill -0 "$cgroup_writer_pid" 2>/dev/null && \
      test "$writer_wait_count" -lt 500
  do
    /usr/bin/sleep 0.01
    writer_wait_count=$((writer_wait_count + 1))
  done
  test -e "$writer_ready" || exit 1
  exited_cgroup_pid=$cgroup_exit_pid
  printf 'x\n' >&7
  wait "$cgroup_exit_pid" || cgroup_synthetic=wrong
  cgroup_exit_pid=
  exec 7>&-
  has_cgroup_exit_descriptor=no
  printf 'x\n' >&6
  wait "$cgroup_writer_pid" || cgroup_synthetic=wrong
  cgroup_writer_pid=
  exec 6>&-
  has_cgroup_writer_descriptor=no
  wait "$cgroup_report_pid" || cgroup_synthetic=wrong
  cgroup_report_pid=
  case $(command cat "$race_report") in
  *" $exited_cgroup_pid "*) cgroup_synthetic=wrong ;;
  esac
else
  synthetic_report=$cgroup_work/release-report
  KOSH_TEST_CGROUP_PROC=$cgroup_work \
    "$BIN" -c 'koshkit --color never eviliso -a -c' \
    > "$synthetic_report" || cgroup_synthetic=wrong
  case $(command cat "$synthetic_report") in
  *kosh-test-exact*) cgroup_synthetic=wrong ;;
  esac
fi
printf 'cgroup-synthetic=%s\n' "$cgroup_synthetic"
if test "$cgroup_synthetic" != matched; then
  command cat "$synthetic_report"
fi

help=$($BIN -c 'koshkit eviliso --help')
case $help in
  *"--all"*"--namespaces"*"--cgroups"*"--sessions"*"--remote"*"--runtime"*)
    help_shape=matched
    ;;
  *) help_shape=wrong ;;
esac
case $help in
*"--detail"*) help_shape=wrong ;;
esac
printf 'help-shape=%s\n' "$help_shape"

"$BIN" -c 'koshkit evilps >/dev/null; koshkit evilss -x >/dev/null; koshkit eviliso -n >/dev/null'
echo "unprivileged-status=$?"
