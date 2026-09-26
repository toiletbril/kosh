#!/bin/sh

work=$TEST_MKTEMP_DIRECTORY/eviliso-loopback-$$
helper_pid=
mismatch_pid=
exit_pid=
synthetic_report_pid=
socket_writer_pid=
has_release_descriptor=no
has_mismatch_descriptor=no
has_exit_descriptor=no
has_socket_writer_descriptor=no

cleanup()
{
  if test -n "$helper_pid"; then
    kill "$helper_pid" 2>/dev/null
    wait "$helper_pid" 2>/dev/null
    helper_pid=
  fi
  for child_pid in "$synthetic_report_pid" "$socket_writer_pid" \
      "$mismatch_pid" "$exit_pid"; do
    if test -n "$child_pid"; then
      kill "$child_pid" 2>/dev/null
      wait "$child_pid" 2>/dev/null
    fi
  done
  if test "$has_mismatch_descriptor" = yes; then
    exec 7>&-
    has_mismatch_descriptor=no
  fi
  if test "$has_exit_descriptor" = yes; then
    exec 8>&-
    has_exit_descriptor=no
  fi
  if test "$has_socket_writer_descriptor" = yes; then
    exec 6>&-
    has_socket_writer_descriptor=no
  fi
  if test "$has_release_descriptor" = yes; then
    exec 9>&-
    has_release_descriptor=no
  fi
  if test -n "$work" && test -d "$work"; then
    "$TEST_SYSTEM_RM" -rf -- "$work"
  fi
}

write_socket_records()
{
  output_descriptor=$1
  printf '%s\n' \
    'header' \
    "0: 0100007F:2711 0200007F:4E21 01 00000001:00000002 00:00000000 00000000 $ready_uid 0 5101" \
    "1: 0100007F:2712 0200007F:4E22 01 00000003:00000004 00:00000000 00000000 $ready_uid 0 0" \
    '2: 0100007F:2713 0200007F:4E23 01 00000005:00000006 00:00000000 00000000 x 0 5103' \
    "3: 0100007F:2714 0200007F:4E24 01 00000007:00000008 00:00000000 00000000 $ready_uid 0 5104" \
    "4: 0100007F:2715 0200007F:4E25 01 00000009:0000000A 00:00000000 00000000 $ready_uid 0 5105" \
    "5: 0100007F:2716 0200007F:4E26 01 0000000B:0000000C 00:00000000 00000000 $ready_uid 0 5106" \
    "6: 0100007F:2717 0200007F:4E27 01 0000000D:0000000E 00:00000000 00000000 $ready_uid 0 5107" \
    >&"$output_descriptor"
}

make_socket_owner()
{
  root=$1
  process_id=$2
  descriptor=$3
  identity=$4
  mkdir -p "$root/$process_id/fd" || return 1
  ln -s "socket:[$identity]" "$root/$process_id/fd/$descriptor"
}

validate_synthetic_report()
{
  synthetic_report=$1
  shared_count=0
  zero_count=0
  missing_uid_count=0
  missing_pid_count=0
  inaccessible_count=0
  mismatch_count=0
  exit_count=0
  has_shared_driver=no
  has_shared_helper=no
  failure=

  case $(command cat "$synthetic_report") in
  *'Remote sockets'*'7'*'Total sockets'*'7'*) ;;
  *) failure=summary ;;
  esac

  while IFS= read -r row; do
    set -- $row
    test "$#" -ge 11 || continue
    test "$1" = IPv4 && test "$2" = TCP && test "$3" = ESTAB || continue
    case $7 in
    127.0.0.2:20001)
      test "$4" = 2 && test "$5" = 1 && test "$8" = 5101 && \
        test "${10}" = "$ready_uid" || failure=shared
      case ${9} in
      "$$") has_shared_driver=yes ;;
      "$ready_pid") has_shared_helper=yes ;;
      *) failure=shared ;;
      esac
      shared_count=$((shared_count + 1))
      ;;
    127.0.0.2:20002)
      test "$4" = 4 && test "$5" = 3 && test "$8" = - && \
        test "${9}" = - && test "${10}" = "$ready_uid" && \
        test "${11}" != - && test "${12}" = - && test "${13}" = - && \
        test "${14}" != - && test "${15}" = - && test "${16}" = - && \
        test "${17}" = - && test "${18}" = - || failure=zero
      zero_count=$((zero_count + 1))
      ;;
    127.0.0.2:20003)
      test "$4" = 6 && test "$5" = 5 && test "$8" = 5103 && \
        test "${9}" = "$ready_pid" && test "${10}" = "$ready_uid" && \
        test "${11}" = "$ready_user" && \
        test "${12}" = eviliso-loop && test "${13}" = ./eviliso-loop && \
        test "${14}" = "$ready_netns" && \
        test "${15}" = "$ready_orchestrator" && \
        test "${16}" = "$ready_runtime" && \
        test "${17}" = "$ready_container" && \
        test "${18}" = "$ready_cgroups" || \
        failure=missing-uid
      missing_uid_count=$((missing_uid_count + 1))
      ;;
    127.0.0.2:20004)
      test "$4" = 8 && test "$5" = 7 && test "$8" = 5104 && \
        test "${9}" = - && test "${10}" = "$ready_uid" && \
        test "${11}" != - && test "${12}" = - && test "${13}" = - && \
        test "${14}" != - && test "${15}" = - && test "${16}" = - && \
        test "${17}" = - && test "${18}" = - || \
        failure=missing-pid
      missing_pid_count=$((missing_pid_count + 1))
      ;;
    127.0.0.2:20005)
      test "$4" = 10 && test "$5" = 9 && test "$8" = 5105 && \
        test "${9}" = 4000000000 && test "${10}" = "$ready_uid" && \
        test "${11}" != - && test "${12}" = - && test "${13}" = - && \
        test "${14}" != - && test "${15}" = - && test "${16}" = - && \
        test "${17}" = - && test "${18}" = - || \
        failure=inaccessible
      inaccessible_count=$((inaccessible_count + 1))
      ;;
    127.0.0.2:20006)
      test "$4" = 12 && test "$5" = 11 && test "$8" = 5106 && \
        test "${9}" = "$mismatch_pid" && test "${10}" = "$ready_uid" && \
        test "${11}" != - && test "${12}" = - && test "${13}" = - && \
        test "${14}" != - && test "${15}" = - && test "${16}" = - && \
        test "${17}" = - && test "${18}" = - || \
        failure=mismatch
      mismatch_count=$((mismatch_count + 1))
      ;;
    127.0.0.2:20007)
      test "$4" = 14 && test "$5" = 13 && test "$8" = 5107 && \
        test "${9}" = "$exited_pid" && test "${10}" = "$ready_uid" && \
        test "${11}" != - && \
        test "${14}" != - || \
        failure=exit
      exit_count=$((exit_count + 1))
      ;;
    esac
  done < "$synthetic_report"

  if test "$shared_count" -eq 2 && test "$has_shared_driver" = yes && \
      test "$has_shared_helper" = yes && test "$zero_count" -eq 1 && \
      test "$missing_uid_count" -eq 1 && \
      test "$missing_pid_count" -eq 1 && \
      test "$inaccessible_count" -eq 1 && test "$mismatch_count" -eq 1 && \
      test "$exit_count" -eq 1 && test -z "$failure"; then
    return 0
  fi

  printf '%s\n' \
    "synthetic counts: shared=$shared_count zero=$zero_count missing-uid=$missing_uid_count missing-pid=$missing_pid_count inaccessible=$inaccessible_count mismatch=$mismatch_count exit=$exit_count failure=$failure mismatch-pid=$mismatch_pid exited-pid=$exited_pid" \
    >&2
  return 1
}

validate_rows()
{
  family=$1
  address=$2
  server_port=$3
  client_port=$4
  server_inode=$5
  client_inode=$6
  rows=$7
  row_count=0
  has_server=no
  has_client=no
  is_valid=yes

  while IFS= read -r row; do
    test -n "$row" || continue
    set -- $row
    if test "$8" != "$server_inode" && test "$8" != "$client_inode"; then
      continue
    fi
    row_count=$((row_count + 1))
    if test "$#" -ne 18 || test "$1" != "$family" || \
       test "$2" != TCP || test "$3" != ESTAB || test "$4" != 0 || \
       test "$5" != 0 || test "${9}" != "$ready_pid" || \
       test "${10}" != "$ready_uid" || test "${11}" != "$ready_user" || \
       test "${12}" != eviliso-loop || \
       test "${13}" != ./eviliso-loop || \
       test "${14}" != "$ready_netns" || \
       test "${15}" != "$ready_orchestrator" || \
       test "${16}" != "$ready_runtime" || \
       test "${17}" != "$ready_container" || \
       test "${18}" != "$ready_cgroups"
    then
      is_valid=no
      continue
    fi

    if test "$8" = "$server_inode" && \
       test "$6" = "$address:$server_port" && \
       test "$7" = "$address:$client_port"
    then
      has_server=yes
    elif test "$8" = "$client_inode" && \
         test "$6" = "$address:$client_port" && \
         test "$7" = "$address:$server_port"
    then
      has_client=yes
    else
      is_valid=no
    fi
  done < "$rows"

  test "$row_count" -eq 2 && test "$has_server" = yes && \
    test "$has_client" = yes && test "$server_port" != "$client_port" && \
    test "$server_inode" != "$client_inode" && test "$is_valid" = yes
}

validate_unix_rows()
{
  rows=$1
  row_count=0
  has_listener=no
  has_client=no
  has_accepted=no
  has_unconnected=no
  client_peer=
  accepted_peer=
  is_valid=yes

  while IFS= read -r row; do
    set -- $row
    test "$#" -eq 9 || continue
    test "$7" = "$ready_pid" || continue
    case $5 in
    "$unix_listener_path:$unix_listener_inode")
      test "$1" = u_str && test "$2" = LISTEN && test "$6" = '*:*' || \
        is_valid=no
      has_listener=yes
      ;;
    "$unix_client_path:$unix_client_inode")
      test "$1" = u_str && test "$2" = ESTAB || is_valid=no
      client_peer=$6
      has_client=yes
      ;;
    "$unix_listener_path:$unix_accepted_inode"|"*:$unix_accepted_inode")
      test "$1" = u_str && test "$2" = ESTAB || is_valid=no
      accepted_peer=$6
      has_accepted=yes
      ;;
    "$unix_unconnected_path:$unix_unconnected_inode")
      test "$1" = u_str && test "$2" = UNCONN && test "$6" = '*:*' || \
        is_valid=no
      has_unconnected=yes
      ;;
    *) continue ;;
    esac
    row_count=$((row_count + 1))
  done < "$rows"

  has_peer_contract=no
  if test "$client_peer" = "*:$unix_accepted_inode" && \
     test "$accepted_peer" = "*:$unix_client_inode"
  then
    has_peer_contract=yes
  fi

  test "$row_count" -eq 4 && test "$has_listener" = yes && \
    test "$has_client" = yes && test "$has_accepted" = yes && \
    test "$has_unconnected" = yes && test "$has_peer_contract" = yes && \
    test "$is_valid" = yes
}

trap cleanup EXIT
mkdir -p "$work" || exit 1
ready=$work/ready
release=$work/release
mkfifo "$ready" "$release" || exit 1
exec 9<> "$release" || exit 1
has_release_descriptor=yes
ln -s "$TEST_EVILISO_LOOPBACK" "$work/eviliso-loop" || exit 1

(cd "$work" && EVILISO_RELEASE=$release exec ./eviliso-loop > "$ready") &
helper_pid=$!
IFS=' ' read -r marker ready_pid ready_uid ready_user ready_netns \
  ipv4_server_port ipv4_client_port ipv4_server_inode ipv4_client_inode \
  has_ipv6 ipv6_server_port ipv6_client_port ipv6_server_inode \
  ipv6_client_inode forward_input_server_port forward_input_client_port \
  forward_input_server_inode forward_input_client_inode \
  forward_output_server_port forward_output_client_port \
  forward_output_server_inode forward_output_client_inode \
  unix_listener_path unix_listener_inode unix_client_path unix_client_inode \
  unix_accepted_inode unix_unconnected_path unix_unconnected_inode \
  ready_orchestrator ready_runtime ready_container ready_cgroups < "$ready"

is_ready=no
case $has_ipv6 in
0|1) is_ready=yes ;;
esac
if test "$marker" = READY && test "$ready_pid" = "$helper_pid" && \
   test "$is_ready" = yes && test "$ipv4_server_port" -gt 0 && \
   test "$ipv4_client_port" -gt 0 && test "$ipv4_server_inode" -gt 0 && \
   test "$ipv4_client_inode" -gt 0 && test -n "$ready_orchestrator" && \
   test "$forward_input_server_port" -gt 0 && \
   test "$forward_input_client_port" -gt 0 && \
   test "$forward_input_server_inode" -gt 0 && \
   test "$forward_input_client_inode" -gt 0 && \
   test "$forward_output_server_port" -gt 0 && \
   test "$forward_output_client_port" -gt 0 && \
   test "$forward_output_server_inode" -gt 0 && \
   test "$forward_output_client_inode" -gt 0 && \
   test "$unix_listener_inode" -gt 0 && test "$unix_client_inode" -gt 0 && \
   test "$unix_accepted_inode" -gt 0 && \
   test "$unix_unconnected_inode" -gt 0 && \
   test -n "$ready_runtime" && test -n "$ready_container" && \
   test -n "$ready_cgroups"
then
  echo 'helper-ready=matched'
else
  echo 'helper-ready=wrong'
fi

report=$work/report
report_status=0
"$BIN" -c 'koshkit --color never eviliso --remote --all' > "$report" || \
  report_status=$?
ipv4_rows=$work/ipv4-rows
ipv6_rows=$work/ipv6-rows
: > "$ipv4_rows"
: > "$ipv6_rows"
has_unix_remote=no
while IFS= read -r row; do
  set -- $row
  test "$#" -eq 18 || continue
  test "${9}" = "$ready_pid" || continue
  case $8 in
  "$unix_listener_inode"|"$unix_client_inode"|"$unix_accepted_inode"|\
  "$unix_unconnected_inode") has_unix_remote=yes ;;
  esac
  case $1 in
  IPv4) printf '%s\n' "$row" >> "$ipv4_rows" ;;
  IPv6) printf '%s\n' "$row" >> "$ipv6_rows" ;;
  esac
done < "$report"

unix_report=$work/unix-report
unix_status=0
"$BIN" -c 'koshkit --color never evilss -xapH' > "$unix_report" || \
  unix_status=$?
unix_rows=$work/unix-rows
: > "$unix_rows"
while IFS= read -r row; do
  set -- $row
  test "$#" -eq 9 && test "$7" = "$ready_pid" || continue
  printf '%s\n' "$row" >> "$unix_rows"
done < "$unix_report"

synthetic_root=$work/synthetic-proc
mkdir -p "$synthetic_root/net" || exit 1
: > "$synthetic_root/net/tcp6"
: > "$synthetic_root/net/udp"
: > "$synthetic_root/net/udp6"
: > "$synthetic_root/net/unix"
synthetic_status=0
if test "${IS_NONDEBUG_BUILD:-0}" = 0; then
  mismatch_release=$work/mismatch-release
  exit_release=$work/exit-release
  socket_writer_release=$work/socket-writer-release
  socket_writer_ready=$work/socket-writer-ready
  mkfifo "$mismatch_release" "$exit_release" "$socket_writer_release" \
    "$synthetic_root/net/tcp" || exit 1
  exec 6<> "$socket_writer_release" || exit 1
  has_socket_writer_descriptor=yes
  exec 7<> "$mismatch_release" || exit 1
  has_mismatch_descriptor=yes
  exec 8<> "$exit_release" || exit 1
  has_exit_descriptor=yes
  (IFS= read -r release_byte < "$mismatch_release") &
  mismatch_pid=$!
  (IFS= read -r release_byte < "$exit_release") &
  exit_pid=$!

  make_socket_owner "$synthetic_root" "$$" 3 5101 || exit 1
  make_socket_owner "$synthetic_root" "$ready_pid" 3 5101 || exit 1
  make_socket_owner "$synthetic_root" "$ready_pid" 4 5103 || exit 1
  make_socket_owner "$synthetic_root" "$mismatch_pid" 3 5106 || exit 1
  make_socket_owner "$synthetic_root" "$exit_pid" 3 5107 || exit 1
  make_socket_owner "$synthetic_root" 4000000000 3 5105 || exit 1
  driver_stat=$(command cat "/proc/$$/stat") || exit 1
  helper_stat=$(command cat "/proc/$ready_pid/stat") || exit 1
  exit_stat=$(command cat "/proc/$exit_pid/stat") || exit 1
  printf '%s\n' "$driver_stat" > "$synthetic_root/$$/stat"
  printf '%s\n' "$helper_stat" > "$synthetic_root/$ready_pid/stat"
  printf '%s\n' "$exit_stat" > "$synthetic_root/$exit_pid/stat"
  printf '%s\n' \
    "$mismatch_pid (fake) S 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 1" \
    > "$synthetic_root/$mismatch_pid/stat"

  synthetic_report=$work/synthetic-report
    KOSH_TEST_SOCKET_PROC=$synthetic_root \
    "$BIN" -c 'koshkit --color never eviliso --remote --all' \
    > "$synthetic_report" &
  synthetic_report_pid=$!
  (
    exec 4> "$synthetic_root/net/tcp" || exit 1
    : > "$socket_writer_ready"
    IFS= read -r release_byte <&6 || exit 1
    write_socket_records 4
  ) &
  socket_writer_pid=$!
  socket_writer_wait_count=0
  while test ! -e "$socket_writer_ready" && \
      kill -0 "$synthetic_report_pid" 2>/dev/null && \
      kill -0 "$socket_writer_pid" 2>/dev/null && \
      test "$socket_writer_wait_count" -lt 500
  do
    /usr/bin/sleep 0.01
    socket_writer_wait_count=$((socket_writer_wait_count + 1))
  done
  test -e "$socket_writer_ready" || exit 1
  exited_pid=$exit_pid
  printf 'x\n' >&8
  wait "$exit_pid" || synthetic_status=$?
  exit_pid=
  "$TEST_SYSTEM_RM" -f -- "$synthetic_root/$exited_pid/stat"
  exec 8>&-
  has_exit_descriptor=no
  printf 'x\n' >&6
  wait "$socket_writer_pid" || synthetic_status=$?
  socket_writer_pid=
  exec 6>&-
  has_socket_writer_descriptor=no
  wait "$synthetic_report_pid" || synthetic_status=$?
  synthetic_report_pid=
  validate_synthetic_report "$synthetic_report" || synthetic_status=$?
  printf 'x\n' >&7
  wait "$mismatch_pid" || synthetic_status=$?
  mismatch_pid=
  exec 7>&-
  has_mismatch_descriptor=no
else
  exec 6> "$synthetic_root/net/tcp"
  write_socket_records 6
  exec 6>&-
  synthetic_report=$work/synthetic-release-report
  KOSH_TEST_SOCKET_PROC=$synthetic_root \
    "$BIN" -c 'koshkit --color never eviliso --remote --all' \
    > "$synthetic_report" || synthetic_status=$?
  case $(command cat "$synthetic_report") in
  *'127.0.0.2:20001'*) synthetic_status=1 ;;
  esac
fi
if test "$synthetic_status" -eq 0; then
  echo 'socket-proc-contract=matched'
else
  echo 'socket-proc-contract=wrong'
  command cat "$synthetic_report"
fi

printf x >&9
helper_status=0
wait "$helper_pid" || helper_status=$?
helper_pid=
if test "$helper_status" -eq 0; then
  echo 'helper-release=matched'
else
  echo 'helper-release=wrong'
fi

if test "$report_status" -eq 0 && \
  validate_rows IPv4 127.0.0.1 "$ipv4_server_port" "$ipv4_client_port" \
  "$ipv4_server_inode" "$ipv4_client_inode" "$ipv4_rows"
then
  echo 'ipv4-remote-rows=matched'
else
  echo 'ipv4-remote-rows=wrong'
  command cat "$ipv4_rows"
  command cat "$report"
fi

forward_status=0
validate_rows IPv4 127.0.0.1 "$forward_input_server_port" \
  "$forward_input_client_port" "$forward_input_server_inode" \
  "$forward_input_client_inode" "$ipv4_rows" || forward_status=$?
validate_rows IPv4 127.0.0.1 "$forward_output_server_port" \
  "$forward_output_client_port" "$forward_output_server_inode" \
  "$forward_output_client_inode" "$ipv4_rows" || forward_status=$?
if test "$forward_status" -eq 0; then
  echo 'forwarded-ipv4-rows=matched'
else
  echo 'forwarded-ipv4-rows=wrong'
  command cat "$ipv4_rows"
fi

if test "$unix_status" -eq 0 && validate_unix_rows "$unix_rows"; then
  echo 'evilss-unix-states=matched'
  echo 'evilss-unix-peers=matched'
else
  echo 'evilss-unix-states=wrong'
  echo 'evilss-unix-peers=wrong'
  command cat "$unix_rows"
fi

if test "$has_unix_remote" = no; then
  echo 'eviliso-unix-exclusion=matched'
else
  echo 'eviliso-unix-exclusion=wrong'
  command cat "$report"
fi

ipv6_status=0
if test "$has_ipv6" -eq 1; then
  validate_rows IPv6 '[::1]' "$ipv6_server_port" "$ipv6_client_port" \
    "$ipv6_server_inode" "$ipv6_client_inode" "$ipv6_rows" || ipv6_status=$?
else
  test ! -s "$ipv6_rows" || ipv6_status=1
fi
if test "$ipv6_status" -eq 0; then
  echo 'ipv6-remote-contract=matched'
else
  echo 'ipv6-remote-contract=wrong'
  command cat "$ipv6_rows"
  command cat "$report"
fi
