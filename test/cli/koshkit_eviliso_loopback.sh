#!/bin/sh

work=$TEST_MKTEMP_DIRECTORY/eviliso-loopback-$$
helper_pid=
has_release_descriptor=no

cleanup()
{
  if test -n "$helper_pid"; then
    kill "$helper_pid" 2>/dev/null
    wait "$helper_pid" 2>/dev/null
    helper_pid=
  fi
  if test "$has_release_descriptor" = yes; then
    exec 9>&-
    has_release_descriptor=no
  fi
  if test -n "$work" && test -d "$work"; then
    "$TEST_SYSTEM_RM" -rf -- "$work"
  fi
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
    test "$#" -eq 7 || continue
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
"$BIN" -c 'koshkit --color never eviliso --remote' > "$report" || \
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
  case $row in
  *" $ready_pid") printf '%s\n' "$row" >> "$unix_rows" ;;
  esac
done < "$unix_report"

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
