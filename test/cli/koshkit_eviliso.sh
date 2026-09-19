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

has_namespace_section()
{
  printf '%s\n' "$1" | command grep -E \
    '^TYPE +ID +(PROCESSES|PID +NAME +ROLE) *$' \
    > "$TEST_NULL_DEVICE" 2>&1
}

has_session_section()
{
  printf '%s\n' "$1" | command grep -E \
    '^USER +TERMINAL( +LOGIN TIME)? *$' > "$TEST_NULL_DEVICE" 2>&1
}

default_report=$(run_report "")
default_shape=matched
case $default_report in
  *"Remote sockets:"*"Runtime:"*)
    default_shape=matched
    ;;
  *) default_shape=missing ;;
esac
has_namespace_section "$default_report" || default_shape=missing
has_cgroup_section "$default_report" || default_shape=missing
has_session_section "$default_report" || default_shape=missing
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
  'namespaces|-n|namespace-section' \
  'cgroups|-c|cgroup-section' \
  'sessions|-s|session-section' \
  'remote|-r|Remote sockets:' \
  'runtime|-k|Runtime:'; do
  old_ifs=$IFS
  IFS='|'
  set -- $selector_section
  IFS=$old_ifs
  report=$(run_report "$2")
  if test "$1" = namespaces; then
    if has_namespace_section "$report"; then
      selector_status=matched
    else
      selector_status=missing
    fi
  elif test "$1" = cgroups; then
    if has_cgroup_section "$report"; then
      selector_status=matched
    else
      selector_status=missing
    fi
  elif test "$1" = sessions; then
    if has_session_section "$report"; then
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
    *"HIERARCHY"*|*"Remote sockets:"*|*"Runtime:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  cgroups)
    case $report in
    *"Remote sockets:"*|*"Runtime:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  sessions)
    case $report in
    *"HIERARCHY"*|*"Remote sockets:"*|*"Runtime:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  remote)
    case $report in
    *"HIERARCHY"*|*"Runtime:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  runtime)
    case $report in
    *"HIERARCHY"*|*"Remote sockets:"*)
      selector_scope=wrong
      ;;
    esac
    ;;
  esac
  if test "$1" != namespaces && has_namespace_section "$report"; then
    selector_scope=wrong
  fi
  if test "$1" != sessions && has_session_section "$report"; then
    selector_scope=wrong
  fi
  printf '%s-scope=%s\n' "$1" "$selector_scope"
done

combined_report=$(run_report '-n -k')
case $combined_report in
*"Runtime:"*) combined_scope=matched ;;
*) combined_scope=missing ;;
esac
has_namespace_section "$combined_report" || combined_scope=missing
case $combined_report in
*"HIERARCHY"*|*"Remote sockets:"*) combined_scope=wrong ;;
esac
if has_session_section "$combined_report"; then combined_scope=wrong; fi
printf 'combined-scope=%s\n' "$combined_scope"

namespace_detail=$(run_report '-a -n')
if has_namespace_section "$namespace_detail"; then
  all_scope=matched
else
  all_scope=missing
fi
case $namespace_detail in
*"HIERARCHY"*|*"Remote sockets:"*|*"Runtime:"*) all_scope=wrong ;;
esac
if has_session_section "$namespace_detail"; then all_scope=wrong; fi
printf 'all-scope=%s\n' "$all_scope"

all_report=$(run_report -a)
case $all_report in
*"HIERARCHY"*"Remote sockets:"*"Runtime:"*)
  all_default_scope=matched
  ;;
*"Membership: unavailable"*"Remote sockets:"*"Runtime:"*)
  all_default_scope=matched
  ;;
*) all_default_scope=missing ;;
esac
has_namespace_section "$all_report" || all_default_scope=missing
has_session_section "$all_report" || all_default_scope=missing
printf 'all-default-scope=%s\n' "$all_default_scope"

namespace_synthetic=matched
if test "${OS-}" != Windows_NT; then
  namespace_root=$cgroup_work/namespaces
  namespace_processes=$cgroup_work/namespace-processes
  mkdir -p \
    "$namespace_root/self/ns" \
    "$namespace_root/3/ns" \
    "$namespace_root/11/ns" \
    "$namespace_root/17/ns" \
    "$namespace_root/29/ns" \
    "$namespace_root/31/ns" || exit 1
  ln -s 'cgroup:[2]' "$namespace_root/self/ns/cgroup" || exit 1
  ln -s 'cgroup:[10]' "$namespace_root/3/ns/cgroup" || exit 1
  ln -s 'cgroup:[2]' "$namespace_root/11/ns/cgroup" || exit 1
  ln -s 'cgroup:[2]' "$namespace_root/17/ns/cgroup" || exit 1
  ln -s 'net:[7]' "$namespace_root/11/ns/net" || exit 1
  printf '%s\n' \
    '17|kosh-e06-ns-self|self' \
    '11|kosh-e06-ns-alpha|other' \
    '3|kosh-e06-ns-zeta|other' \
    '29|kosh-e06-ns-denied|other' \
    '31|kosh-e06-ns-exited|other' \
    > "$namespace_processes"
  if test "${IS_NONDEBUG_BUILD:-0}" = 0; then
    namespace_summary=$cgroup_work/namespace-summary
    KOSH_TEST_EVILISO_NAMESPACE_PROC=$namespace_root \
      KOSH_TEST_EVILISO_NAMESPACE_PROCESSES=$namespace_processes \
      "$BIN" -c 'koshkit --color never eviliso -n' \
      > "$namespace_summary" || namespace_synthetic=wrong
    namespace_summary_header=no
    namespace_summary_rows=
    namespace_net_rows=
    while IFS= read -r row; do
      set -- $row
      if test "$#" -eq 3 && test "$1" = TYPE && test "$2" = ID && \
          test "$3" = PROCESSES; then
        namespace_summary_header=yes
        continue
      fi
      test "$#" -eq 3 && test "$1" = cgroup || continue
      if test -n "$namespace_summary_rows"; then
        namespace_summary_rows="$namespace_summary_rows|"
      fi
      namespace_summary_rows="$namespace_summary_rows$2:$3"
    done < "$namespace_summary"
    test "$namespace_summary_header" = yes && \
      test "$namespace_summary_rows" = \
      '2:2|10:1|unavailable:2' || namespace_synthetic=wrong
    while IFS= read -r row; do
      set -- $row
      test "$#" -eq 3 && test "$1" = net || continue
      if test -n "$namespace_net_rows"; then
        namespace_net_rows="$namespace_net_rows|"
      fi
      namespace_net_rows="$namespace_net_rows$2:$3"
    done < "$namespace_summary"
    test "$namespace_net_rows" = '7:1|unavailable:4' || \
      namespace_synthetic=wrong

    namespace_detail_report=$cgroup_work/namespace-detail
    KOSH_TEST_EVILISO_NAMESPACE_PROC=$namespace_root \
      KOSH_TEST_EVILISO_NAMESPACE_PROCESSES=$namespace_processes \
      "$BIN" -c 'koshkit --color never eviliso -a -n' \
      > "$namespace_detail_report" || namespace_synthetic=wrong
    namespace_detail_header=no
    namespace_detail_rows=
    namespace_net_detail=no
    while IFS= read -r row; do
      set -- $row
      if test "$#" -eq 5 && test "$1" = TYPE && test "$2" = ID && \
          test "$3" = PID && test "$4" = NAME && test "$5" = ROLE; then
        namespace_detail_header=yes
        continue
      fi
      test "$#" -eq 5 && test "$1" = cgroup || continue
      if test -n "$namespace_detail_rows"; then
        namespace_detail_rows="$namespace_detail_rows|"
      fi
      namespace_detail_rows="$namespace_detail_rows$2:$3:$4:$5"
    done < "$namespace_detail_report"
    test "$namespace_detail_header" = yes && \
      test "$namespace_detail_rows" = \
        '2:11:kosh-e06-ns-alpha:other|2:17:kosh-e06-ns-self:self|10:3:kosh-e06-ns-zeta:other|unavailable:29:kosh-e06-ns-denied:other|unavailable:31:kosh-e06-ns-exited:other' || \
      namespace_synthetic=wrong
    while IFS= read -r row; do
      set -- $row
      if test "$#" -eq 5 && test "$1" = net && test "$2" = 7 && \
          test "$3" = 11 && test "$4" = kosh-e06-ns-alpha && \
          test "$5" = other; then
        namespace_net_detail=yes
      fi
    done < "$namespace_detail_report"
    test "$namespace_net_detail" = yes || namespace_synthetic=wrong
    case $(command cat "$namespace_detail_report") in
    *"cgroup:"*|*"cgroup process:"*) namespace_synthetic=wrong ;;
    esac

    namespace_unsupported_root=$cgroup_work/namespace-unsupported-root
    namespace_unsupported_report=$cgroup_work/namespace-unsupported-report
    mkdir -p "$namespace_unsupported_root" || exit 1
    KOSH_TEST_EVILISO_NAMESPACE_PROC=$namespace_unsupported_root \
      KOSH_TEST_EVILISO_NAMESPACE_PROCESSES=$namespace_processes \
      "$BIN" -c 'koshkit --color never eviliso -a -n' \
      > "$namespace_unsupported_report" || namespace_synthetic=wrong
    namespace_unsupported_count=0
    while IFS= read -r row; do
      set -- $row
      test "$1" = TYPE && continue
      test "$#" -eq 5 && test "$2" = unavailable && \
        test "$4" = - && test "$5" = self || namespace_synthetic=wrong
      namespace_unsupported_count=$((namespace_unsupported_count + 1))
    done < "$namespace_unsupported_report"
    test "$namespace_unsupported_count" -eq 8 || namespace_synthetic=wrong
  else
    namespace_release=$cgroup_work/namespace-release
    KOSH_TEST_EVILISO_NAMESPACE_PROC=$namespace_root \
      KOSH_TEST_EVILISO_NAMESPACE_PROCESSES=$namespace_processes \
      "$BIN" -c 'koshkit --color never eviliso -a -n' \
      > "$namespace_release" || namespace_synthetic=wrong
    case $(command cat "$namespace_release") in
    *kosh-e06-ns-self*|*kosh-e06-ns-denied*|*kosh-e06-ns-exited*)
      namespace_synthetic=wrong
      ;;
    esac
  fi
fi
printf 'namespace-synthetic=%s\n' "$namespace_synthetic"
if test "$namespace_synthetic" != matched; then
  command cat "${namespace_summary-$namespace_release}"
  test -z "${namespace_detail_report-}" || command cat "$namespace_detail_report"
fi

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

session_fixture=$cgroup_work/sessions
printf '%s\n' \
  'zeta|pts/9|0' \
  'alpha|tty2|200' \
  'alpha|tty1|100' \
  > "$session_fixture"
session_synthetic=matched
if test "${IS_NONDEBUG_BUILD:-0}" = 0; then
  session_default=$(TZ=UTC KOSH_TEST_EVILISO_SESSIONS=$session_fixture \
    "$BIN" -c 'koshkit --color never eviliso -s')
  expected_session_default=$(printf '%s\n' \
    'USER   TERMINAL' \
    'alpha  tty1    ' \
    'alpha  tty2    ' \
    'zeta   pts/9   ')
  test "$session_default" = "$expected_session_default" || \
    session_synthetic=wrong
  session_detail=$(TZ=UTC KOSH_TEST_EVILISO_SESSIONS=$session_fixture \
    "$BIN" -c 'koshkit --color never eviliso -a -s')
  expected_session_detail=$(printf '%s\n' \
    'USER   TERMINAL  LOGIN TIME         ' \
    'alpha  tty1      1970-01-01 00:01:40' \
    'alpha  tty2      1970-01-01 00:03:20' \
    'zeta   pts/9     unavailable        ')
  test "$session_detail" = "$expected_session_detail" || \
    session_synthetic=wrong
  empty_sessions=$cgroup_work/empty-sessions
  : > "$empty_sessions"
  empty_session_report=$(KOSH_TEST_EVILISO_SESSIONS=$empty_sessions \
    "$BIN" -c 'koshkit --color never eviliso -a -s')
  test "$empty_session_report" = 'USER  TERMINAL  LOGIN TIME' || \
    session_synthetic=wrong
else
  session_report=$(TZ=UTC KOSH_TEST_EVILISO_SESSIONS=$session_fixture \
    "$BIN" -c 'koshkit --color never eviliso -a -s')
  case $session_report in
  *zeta*pts/9*) session_synthetic=wrong ;;
  esac
fi
printf 'session-synthetic=%s\n' "$session_synthetic"
if test "$session_synthetic" != matched; then
  printf 'session-default=<%s>\n' "$session_default"
  printf 'session-detail=<%s>\n' "$session_detail"
  printf 'session-empty=<%s>\n' "${empty_session_report-}"
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
