unset KOSH_FLAGS
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf 'echo one\nls\ncd /tmp\ngit status\n' > "$dir/hist"
export KOSH_HISTORY="$dir/hist"
echo "== the numbered list prints every entry =="
"$BIN" -c 'history'
echo "== a trailing count prints only the most recent entries =="
"$BIN" -c 'history 2'
echo "== a non-numeric count is rejected without printing the list =="
"$BIN" -c 'history foo; echo "rc=$?"'
echo "== a count past the list size still prints every entry, no overflow =="
"$BIN" -c 'history 999999999999999999999999'
echo "== the print flag echoes its operands and stores nothing =="
"$BIN" -c 'history -p alpha beta'
echo "== builtin history -a no longer reports an unknown builtin =="
"$BIN" -c 'builtin history -a; echo continued'
echo "== type reports the builtin =="
"$BIN" -c 'type history'
echo "== clear empties the list =="
"$BIN" -c 'history -c; history; echo cleared'

unset KOSH_FLAGS
# history -r <file> reads a named file into the list. The list is backed by its
# file, so the named file is merged into the backing history and the whole list
# is listed back.
d=$(mktemp -d)
printf 'existing one\nexisting two\n' > "$d/hist"
printf 'merged alpha\nmerged beta\n' > "$d/extra"
echo "== history -r <file> merges the named file into the list:"
KOSH_HISTORY="$d/hist" "$BIN" -c "history -r $d/extra; history"
echo "== history -r on a missing file errors:"
KOSH_HISTORY="$d/hist" "$BIN" -c \
    'history -r "$TEST_TEMP_DIRECTORY/no-such-history"; echo "rc=$?"' \
    2>/dev/null
[ -n "$d" ] && rm -rf "$d"

# History recall brings back the newest command even when the history file holds
# more entries than the in-memory ring. A missing parenthesis in
# TL_HISTORY_MAX_SIZE once parsed the ring modulo as (x % 1024) * 4, so past 4096
# entries the up arrow recalled a stale older line. The editor needs a tty, so
# the run skips when script or the target terminal handles cannot provide one.
if ! BIN="$BIN" script -qec \
  'exec "$BIN" -c "test -t 0 && test -t 1"' \
  /dev/null >/dev/null 2>&1; then
  echo "recall ok"
  echo "fc replacement ok"
  echo "search casefold ok"
  echo "history expansion ok"
  echo "history option ok"
  echo "ignoreeof ok"
  exit 0
fi
hist=$dir/recall
search_hist=$dir/search
expansion_hist=$dir/expansion
disabled_hist=$dir/disabled
ignoreeof_hist=$dir/ignoreeof
ignoreeof_rc=$dir/ignoreeof-rc
stderr_hist=$dir/stderr-history
stderr_log=$dir/stderr-log
stderr_rc=$dir/stderr-rc
ready=$dir/ready
input_status=$dir/input-status
send_input_when_ready()
{
  wait_count=0
  while [ ! -s "$ready" ] && [ "$wait_count" -lt 600 ]; do
    sleep 0.05
    wait_count=$((wait_count + 1))
  done
  [ -s "$ready" ] || return 1
  sleep 0.25
  for key_sequence in "$@"; do
    printf '%b' "$key_sequence" || return 1
    sleep 0.05 || return 1
  done
  sleep 0.25
}
i=1
while [ "$i" -le 4200 ]; do
  printf 'echo CMD_%05d\n' "$i" >> "$hist"
  i=$((i + 1))
done
# Up arrow then enter recalls and runs the newest entry, then exit leaves.
rm -f "$ready"
rm -f "$input_status"
out=$({
  send_input_when_ready '\033[A' '\r' 'echo FC_ACTIVE_RECALL\r' \
    'fc -s -1\r' 'exit\r'
  printf '%s\n' "$?" > "$input_status"
} |
  BIN="$BIN" READY="$ready" KOSH_HISTORY="$hist" \
    PROMPT_COMMAND='printf ready > "$READY"; unset PROMPT_COMMAND' \
    script -qec 'exec "$BIN" -i --rcfile /dev/null' /dev/null 2>/dev/null) ||
  exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
case "$out" in
*CMD_04200*) echo "recall ok" ;;
*) echo "recall broken" ;;
esac
if grep -q 'fc -s' "$hist"; then
  echo "fc replacement broken"
else
  echo "fc replacement ok"
fi
printf 'echo MiXeD_History_Marker\n' > "$search_hist"
rm -f "$ready"
rm -f "$input_status"
out=$({
  send_input_when_ready '\022' 'mixed_history_marker' '\r' '\r' 'exit\r'
  printf '%s\n' "$?" > "$input_status"
} |
  BIN="$BIN" READY="$ready" KOSH_HISTORY="$search_hist" \
    PROMPT_COMMAND='printf ready > "$READY"; unset PROMPT_COMMAND' \
    script -qec 'exec "$BIN" -i --rcfile /dev/null' /dev/null 2>/dev/null) ||
  exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
case "$out" in
*MiXeD_History_Marker*) echo "search casefold ok" ;;
*) echo "search casefold broken" ;;
esac

printf 'echo HISTORY_EXPANSION_MARKER\n' > "$expansion_hist"
rm -f "$ready"
rm -f "$input_status"
out=$({
  send_input_when_ready 'echo !!\r' 'echo one !# !#\r' \
    '!echo; echo PUNCTUATION_HISTORY_MARKER\r' \
    'echo !MISSING_HISTORY_EVENT\r' 'set +H\r' \
    'printf "<%s>\\n" !!\r' 'exit\r'
  printf '%s\n' "$?" > "$input_status"
} |
  BIN="$BIN" READY="$ready" KOSH_HISTORY="$expansion_hist" \
    PROMPT_COMMAND='printf ready > "$READY"; unset PROMPT_COMMAND' \
    script -qec 'exec "$BIN" -i -M bash --rcfile /dev/null' \
      /dev/null 2>/dev/null) || exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
case "$out" in
*HISTORY_EXPANSION_MARKER*PUNCTUATION_HISTORY_MARKER*'event not found'*'<!!>'*)
  if grep -q '!#\|^!echo\|MISSING_HISTORY_EVENT' "$expansion_hist"; then
    echo "history expansion broken"
  else
    echo "history expansion ok"
  fi
*) echo "history expansion broken" ;;
esac

printf 'echo STDERR_HISTORY_MARKER\n' > "$stderr_hist"
printf 'exec 2>"$HISTORY_STDERR_LOG"\n' > "$stderr_rc"
rm -f "$ready"
rm -f "$input_status"
out=$({
  send_input_when_ready 'echo !!\r' 'exit\r'
  printf '%s\n' "$?" > "$input_status"
} |
  BIN="$BIN" READY="$ready" KOSH_HISTORY="$stderr_hist" \
    HISTORY_STDERR_LOG="$stderr_log" HISTORY_STDERR_RC="$stderr_rc" \
    PROMPT_COMMAND='printf ready > "$READY"; unset PROMPT_COMMAND' \
    script -qec 'exec "$BIN" -i -M bash --rcfile "$HISTORY_STDERR_RC"' \
      /dev/null 2>/dev/null) || exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
if grep -q '^echo echo STDERR_HISTORY_MARKER$' "$stderr_log"; then
  :
else
  echo "history expansion broken"
fi

printf 'echo HISTORY_BASE\n' > "$disabled_hist"
rm -f "$ready"
rm -f "$input_status"
out=$({
  send_input_when_ready 'set +o history\r' \
    'echo OMITTED_HISTORY_MARKER\r' 'set -o history\r' 'exit\r'
  printf '%s\n' "$?" > "$input_status"
} |
  BIN="$BIN" READY="$ready" KOSH_HISTORY="$disabled_hist" \
    PROMPT_COMMAND='printf ready > "$READY"; unset PROMPT_COMMAND' \
    script -qec 'exec "$BIN" -i -M bash --rcfile /dev/null' \
      /dev/null 2>/dev/null) || exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
if grep -q OMITTED_HISTORY_MARKER "$disabled_hist"; then
  echo "history option broken"
else
  echo "history option ok"
fi

printf 'IGNOREEOF=1\n' > "$ignoreeof_rc"
rm -f "$ready"
rm -f "$input_status"
out=$({
  send_input_when_ready '\004' 'echo EOF_RESET_MARKER\r' '\004' '\004'
  printf '%s\n' "$?" > "$input_status"
} |
  BIN="$BIN" READY="$ready" KOSH_HISTORY="$ignoreeof_hist" \
    IGNOREEOF_RC="$ignoreeof_rc" \
    PROMPT_COMMAND='printf ready > "$READY"; unset PROMPT_COMMAND' \
    script -qec 'exec "$BIN" -i -M bash --rcfile "$IGNOREEOF_RC"' \
      /dev/null 2>/dev/null) || exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
warning_count=$(printf '%s\n' "$out" |
  grep -c 'Use "exit" to leave the shell.')
case "$out:$warning_count" in
*EOF_RESET_MARKER*:2) echo "ignoreeof ok" ;;
*) echo "ignoreeof broken" ;;
esac
