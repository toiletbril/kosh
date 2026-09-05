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
script_mode=
if script -qec true /dev/null >/dev/null 2>&1; then
  script_mode=gnu
elif script -q /dev/null /usr/bin/true >/dev/null 2>&1; then
  script_mode=bsd
fi
run_interactive()
{
  if [ "$script_mode" = gnu ]; then
    script -qec "$1" /dev/null 2>/dev/null
  else
    script -q /dev/null /bin/sh -c "$1" 2>/dev/null
  fi
}
if [ -z "$script_mode" ] || ! BIN="$BIN" run_interactive \
  'exec "$BIN" -c "test -t 0 && test -t 1"' >/dev/null 2>&1; then
  echo "interactive history coverage requires a usable script terminal" >&2
  exit 1
fi
hist=$dir/recall
search_hist=$dir/search
expansion_hist=$dir/expansion
word_designator_history_path=$dir/words
modifier_history_path=$dir/modifiers
modifier_print_path=$dir/modifier-print
modifier_stderr_log=$dir/modifier-stderr
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
    if ! printf '%b' "$key_sequence"; then
      printf 'failed interactive input: %s\n' "$key_sequence" >&2
      return 1
    fi
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
    run_interactive 'exec "$BIN" -i --rcfile /dev/null') ||
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
    run_interactive 'exec "$BIN" -i --rcfile /dev/null') ||
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
    run_interactive 'exec "$BIN" -i -M bash --rcfile /dev/null') || exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
case "$out" in
*HISTORY_EXPANSION_MARKER*PUNCTUATION_HISTORY_MARKER*'event not found'*'<!!>'*)
  if grep -q '!#\|^!echo\|MISSING_HISTORY_EVENT' "$expansion_hist"; then
    echo "history expansion broken"
  else
    echo "history expansion ok"
  fi
  ;;
*) echo "history expansion broken" ;;
esac

printf '%s\n' 'marker alpha beta gamma needle' \
  'marker <(printf x) tail' > "$word_designator_history_path"
rm -f "$ready"
rm -f "$input_status"
out=$({
  send_input_when_ready \
    'set -- !1:0; printf "W0=%s,%s\\n" "$#" "$1"\r' \
    'set -- !1^; printf "WF=%s,%s\\n" "$#" "$1"\r' \
    'set -- !1:2; printf "WN=%s,%s\\n" "$#" "$1"\r' \
    'set -- !1$; printf "WL=%s,%s\\n" "$#" "$1"\r' \
    'set -- !1*; printf "WA=%s,%s,%s\\n" "$#" "$1" "$4"\r' \
    'set -- !1:1-2; printf "WR=%s,%s,%s\\n" "$#" "$1" "$2"\r' \
    'set -- !1:-2; printf "WR0=%s,%s,%s\\n" "$#" "$1" "$3"\r' \
    'set -- !1:2*; printf "WRS=%s,%s,%s\\n" "$#" "$1" "$3"\r' \
    'set -- !1:2-$; printf "WRD=%s,%s,%s\\n" "$#" "$1" "$3"\r' \
    'set -- !1:2-; printf "WRL=%s,%s,%s\\n" "$#" "$1" "$2"\r' \
    'set -- !?gamma?:%; printf "WP=%s,%s\\n" "$#" "$1"\r' \
    'echo unrelated one two three\r' \
    'printf "WPP=<%s>\\n" "!%"\r' \
    'printf "WPS=<%s>\\n" "!2:^"\r' \
    'printf "WPT=<%s>\\n" "!2:2"\r' \
    'printf "WINHIBIT=<%s>\\n" "!("\r' \
    '!marker=tail\r' \
    'false\r' \
    '!1:99\r' \
    'printf "WSTATUS=%s\\n" "$?"\r' \
    'exit\r'
  printf '%s\n' "$?" > "$input_status"
} |
  BIN="$BIN" READY="$ready" KOSH_HISTORY="$word_designator_history_path" \
    PROMPT_COMMAND='printf ready > "$READY"; unset PROMPT_COMMAND' \
    run_interactive 'exec "$BIN" -i -M bash --rcfile /dev/null') || exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
case "$out" in
*'W0=1,marker'*'WF=1,alpha'*'WN=1,beta'*'WL=1,needle'*\
*'WA=4,alpha,needle'*'WR=2,alpha,beta'*'WR0=3,marker,beta'*\
*'WRS=3,beta,needle'*'WRD=3,beta,needle'*'WRL=2,beta,gamma'*'WP=1,gamma'*\
*'WPP=<gamma>'*'WPS=<<(printf x)>'*'WPT=<tail>'*'WINHIBIT=<!(>'*\
*'!marker=tail: event not found'*':99: bad word specifier'*'WSTATUS=1'*)
  echo "history words ok" ;;
*) echo "history words broken" ;;
esac

printf '%s\n' 'echo /aa/bb/name.tar.gz' \
  'printf "SUB=<%s,%s>\\n" oldold oldold' \
  'printf ARG=%s two words' \
  'printf P_EXECUTED > "$HISTORY_P_MARKER"' \
  'echo SHOULD_NOT_EXPAND' 'echo alpha needle omega' \
  'echo quick old old' > "$modifier_history_path"
rm -f "$ready"
rm -f "$input_status"
out=$({
  send_input_when_ready \
    '!?needle?:p\r' \
    '!?needle?:s//X/:p\r' \
    '!6:p\r' \
    '^^Y\r' \
    '^Y^Z^ tail\r' \
    '^Z^W^:p\r' \
    '!7:s/old/new/\r' \
    'printf "H=<%s>\\n" "!1:$:h"\r' \
    'printf "T=<%s>\\n" "!1:$:t"\r' \
    'printf "R=<%s>\\n" "!1:$:r"\r' \
    'printf "E=<%s>\\n" "!1:$:e"\r' \
    '!2:s/old/new/\r' \
    '!2:&\r' \
    '!2:gs/old/new/\r' \
    '!2:as/old/new/\r' \
    '!2:Gs/old/new/\r' \
    '!2:g&\r' \
    'set -- !3:*:q; printf "Q=%s,<%s>,<%s>\\n" "$#" "$1" "${2-unset}"\r' \
    'set -- !3:*:x; printf "X=%s,<%s>,<%s>\\n" "$#" "$1" "$2"\r' \
    '!4:p\r' \
    'printf "AFTER_P\\n"\r' \
    'printf "SQ=<%s>\\n" '\''!!'\''\r' \
    'printf "AQ=<%s>\\n" $'\''!!'\''\r' \
    'printf "ES=<%s>\\n" \\!\\!\r' \
    'printf "DQ=<%s>\\n" "!"\r' \
    'echo COMMENT_OK # !5\r' \
    'false # DUPLICATE_HISTORY_MARKER\r' \
    '!!:p\r' \
    'printf "PSTATUS=%s\\n" "$?"\r' \
    'false\r' \
    '!2:z\r' \
    'printf "ZMSTATUS=%s\\n" "$?"\r' \
    'false\r' \
    '!2:s/MISSING/repl/\r' \
    'printf "SMSTATUS=%s\\n" "$?"\r' \
    'exit\r'
  printf '%s\n' "$?" > "$input_status"
} |
  BIN="$BIN" READY="$ready" KOSH_HISTORY="$modifier_history_path" \
    HISTORY_P_MARKER="$modifier_print_path" \
    MODIFIER_STDERR_LOG="$modifier_stderr_log" \
    PROMPT_COMMAND='printf ready > "$READY"; unset PROMPT_COMMAND' \
    run_interactive \
      'exec "$BIN" -i -M bash --rcfile /dev/null 2>"$MODIFIER_STDERR_LOG"') ||
  exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
case "$out" in
*'alpha Y omega'*'alpha Z omega tail'*'quick new old'*'H=</aa/bb>'*\
*'T=<name.tar.gz>'*\
*'R=</aa/bb/name.tar>'*'E=<.gz>'*'SUB=<newold,oldold>'*\
*'SUB=<newold,oldold>'*'SUB=<newnew,newnew>'*'SUB=<newnew,newnew>'*\
*'SUB=<newold,newold>'*'SUB=<newnew,newnew>'*\
*'Q=1,<ARG=%s two words>,<unset>'*\
*'X=3,<ARG=%s>,<two>'*'AFTER_P'*'SQ=<!!>'*'AQ=<!!>'*'ES=<!!>'*\
*'DQ=<!>'*'COMMENT_OK'*'PSTATUS=1'*'ZMSTATUS=1'*'SMSTATUS=1'*)
  case "$out" in
  *SHOULD_NOT_EXPAND*|*'alpha W omega tail'*)
    echo "history modifier execution broken"
    ;;
  *)
    if [ -e "$modifier_print_path" ]; then
      echo "history print modifier broken"
    elif [ "$(grep -c '^false # DUPLICATE_HISTORY_MARKER$' \
      "$modifier_history_path")" -ne 2 ]; then
      echo "history duplicate recording broken"
    elif grep -q -e ':z' -e MISSING "$modifier_history_path"; then
      echo "history modifier error recording broken"
    elif ! grep -qx 'printf P_EXECUTED > "$HISTORY_P_MARKER"' \
      "$modifier_stderr_log" ||
      ! grep -qx 'echo alpha W omega tail' "$modifier_stderr_log" ||
      ! grep -qx 'z: unrecognized history modifier' "$modifier_stderr_log" ||
      ! grep -qx ':s/MISSING/repl/: substitution failed' \
        "$modifier_stderr_log"; then
      printf 'history modifier diagnostics:\n%.4096s\n' \
        "$(cat "$modifier_stderr_log")" >&2
      echo "history modifier diagnostic stream broken"
    else
      echo "history modifiers ok"
    fi
    ;;
  esac
  ;;
*)
  printf 'history modifier output:\n%.4096s\n' "$out" >&2
  echo "history modifiers broken"
  ;;
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
    run_interactive \
      'exec "$BIN" -i -M bash --rcfile "$HISTORY_STDERR_RC"') || exit 1
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
    run_interactive 'exec "$BIN" -i -M bash --rcfile /dev/null') || exit 1
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
    run_interactive 'exec "$BIN" -i -M bash --rcfile "$IGNOREEOF_RC"') ||
  exit 1
[ "$(cat "$input_status")" = 0 ] || exit 1
warning_count=$(printf '%s\n' "$out" |
  grep -c 'Use "exit" to leave the shell.')
case "$out:$warning_count" in
*EOF_RESET_MARKER*:2) echo "ignoreeof ok" ;;
*) echo "ignoreeof broken" ;;
esac
