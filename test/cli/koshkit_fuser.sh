d=$(mktemp -d)
holder_pid=
holder_pid_two=
finish()
{
  if [ -n "$d" ]; then
    : > "$d/release"
    if [ -n "$holder_pid" ]; then
      wait "$holder_pid" 2>/dev/null
    fi
    if [ -n "$holder_pid_two" ]; then
      wait "$holder_pid_two" 2>/dev/null
    fi
    "$TEST_SYSTEM_PATH/rm" -r "$d"
  fi
}
trap finish EXIT

: > "$d/held"
: > "$d/unused"
"$BIN" -c 'exec 3<"$1"; : > "$2"; while [ ! -e "$3" ]; do :; done' \
  fuser-holder "$d/held" "$d/ready" "$d/release" &
holder_pid=$!
"$BIN" -c 'exec 3<"$1"; : > "$2"; while [ ! -e "$3" ]; do :; done' \
  fuser-holder-two "$d/held" "$d/ready-two" "$d/release" &
holder_pid_two=$!

attempt_count=0
while [ ! -e "$d/ready" ] && [ "$attempt_count" -lt 1000 ]; do
  sleep 0.01
  attempt_count=$((attempt_count + 1))
done
[ -e "$d/ready" ] || exit 1
attempt_count=0
while [ ! -e "$d/ready-two" ] && [ "$attempt_count" -lt 1000 ]; do
  sleep 0.01
  attempt_count=$((attempt_count + 1))
done
[ -e "$d/ready-two" ] || exit 1
if [ "$holder_pid" -lt "$holder_pid_two" ]; then
  first_pid=$holder_pid
  second_pid=$holder_pid_two
  expected_pids=$holder_pid$holder_pid_two
else
  first_pid=$holder_pid_two
  second_pid=$holder_pid
  expected_pids=$holder_pid_two$holder_pid
fi
expected_combined=$d/held:$first_pid'f'$second_pid'f'

"$BIN" -c 'koshkit fuser "$1"' fuser "$d/held" \
  > "$d/stdout" 2> "$d/stderr"
status=$?
if [ "$status" -eq 0 ] && [ "$(cat "$d/stdout")" = "$expected_pids" ]; then
  echo pid=passed
else
  echo "pid=failed status=$status"
fi
stdout_length=$(wc -c < "$d/stdout")
if [ "$stdout_length" -eq "${#expected_pids}" ]; then
  echo stdout-framing=passed
else
  echo stdout-framing=failed
fi
case $(cat "$d/stderr") in
  "$d/held:"*f*) echo use=passed ;;
  *) echo use=failed ;;
esac

"$BIN" -c 'koshkit fuser -u "$1"' fuser "$d/held" \
  > "$d/user-stdout" 2> "$d/user-stderr"
case $(cat "$d/user-stderr") in
  "$d/held:"*f*\(*\)*) echo user=passed ;;
  *) echo user=failed ;;
esac

"$BIN" -c 'koshkit fuser "$1" "$2"' fuser "$d/held" "$d/unused" \
  > "$d/multiple-stdout" 2> "$d/multiple-stderr"
if [ "$?" -eq 0 ] && [ "$(cat "$d/multiple-stdout")" = "$expected_pids" ]; then
  echo operands=passed
else
  echo operands=failed
fi

"$BIN" -c 'koshkit fuser "$1" > "$2" 2>&1' fuser "$d/held" "$d/combined"
case $(cat "$d/combined") in
  "$expected_combined"*) echo combined=passed ;;
  *) echo combined=failed ;;
esac

"$BIN" -c 'koshkit fuser' > "$d/no-operand" 2>&1
echo "no-operand=$?"
"$BIN" -c 'koshkit fuser "$1"' fuser "$d/unused" \
  > "$d/unused-output" 2>&1
unused_status=$?
if [ "$unused_status" -eq 1 ] && [ ! -s "$d/unused-output" ]; then
  echo unused=passed
else
  echo "unused=failed status=$unused_status"
fi
"$BIN" -c 'koshkit fuser "$1"' fuser "$d/missing" > "$d/missing-output" 2>&1
echo "missing=$?"

if [ "${OS-}" = Windows_NT ]; then
  "$BIN" -c 'koshkit fuser -c "$1"' fuser "$d/held" > "$d/device" 2>&1
  echo "device-unsupported=$?"
else
  "$BIN" -c 'koshkit fuser -c "$1"' fuser "$d/held" \
    > "$d/device-stdout" 2> "$d/device-stderr"
  if [ "$?" -eq 0 ] && [ -s "$d/device-stdout" ]; then
    echo device=passed
  else
    echo device=failed
  fi
fi
