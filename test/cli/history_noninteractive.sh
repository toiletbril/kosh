#!/bin/sh
#
#    This file is a part of the Koshka shell, (c) toiletbril, 2026
#    See the top-level LICENSE file for the licensing information.
#
# This script verifies noninteractive history listing, maintenance, file
# access, multiline storage, and in-memory limit changes.

unset KOSH_FLAGS
dir=$(mktemp -d)
cleanup()
{
  if [ -n "$dir" ]; then
    "$TEST_SYSTEM_RM" -rf -- "$dir"
  fi
}
trap cleanup EXIT

printf 'echo one\nls\ncd /tmp\ngit status\n' > "$dir/hist"
export KOSH_HISTORY_FILE="$dir/hist"
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

printf 'existing one\nexisting two\n' > "$dir/hist"
printf 'merged alpha\nmerged beta\n' > "$dir/extra"
echo "== history -r reads a named file into the list =="
"$BIN" -c 'history -r "$1"; history' history-test "$dir/extra"
echo "== history -r on a missing file errors =="
"$BIN" -c 'history -r "$1"; echo "rc=$?"' history-test \
  "$dir/no-such-history" 2>/dev/null

printf '\001' > "$dir/invalid"
echo "== history -r rejects invalid data without changing the list =="
"$BIN" -c 'history -r "$1"; echo "rc=$?"; history' history-test \
  "$dir/invalid" 2>/dev/null

printf 'tail' > "$dir/unterminated"
printf 'next\n' > "$dir/next"
echo "== history -r separates an unterminated backing record =="
KOSH_HISTORY_FILE="$dir/unterminated" "$BIN" --no-init-files -c \
  'history -r "$1"; history' history-test "$dir/next"

: > "$dir/empty"
echo "== an empty import creates a missing backing file =="
KOSH_HISTORY_FILE="$dir/empty-backing" "$BIN" --no-init-files -c \
  'history -r "$1"; echo "rc=$?"' history-test "$dir/empty"

printf '\377\n' > "$dir/high-byte"
echo "== history accepts high bytes as file data =="
KOSH_HISTORY_FILE="$dir/high-byte-backing" "$BIN" --no-init-files -c \
  'history -r "$1"; echo "rc=$?"; history | koshkit wc -l' history-test \
  "$dir/high-byte"

: > "$dir/oversized"
byte_count=0
while [ "$byte_count" -lt 4096 ]; do
  printf x >> "$dir/oversized"
  byte_count=$((byte_count + 1))
done
printf '\n' >> "$dir/oversized"
echo "== an oversized decoded record is omitted =="
KOSH_HISTORY_FILE="$dir/oversized" "$BIN" --no-init-files -c \
  'history | koshkit wc -l'

: > "$dir/concurrent"
echo "== concurrent history stores preserve both records =="
KOSH_HISTORY_FILE="$dir/concurrent" "$BIN" --no-init-files -c \
  'history -s first' &
first_pid=$!
KOSH_HISTORY_FILE="$dir/concurrent" "$BIN" --no-init-files -c \
  'history -s second' &
second_pid=$!
wait "$first_pid"
wait "$second_pid"
KOSH_HISTORY_FILE="$dir/concurrent" "$BIN" --no-init-files -c \
  'history | koshkit wc -l'

: > "$dir/synchronized"
echo "== running shells reload history written by another instance =="
KOSH_HISTORY_FILE="$dir/synchronized" "$BIN" --no-init-files -c \
  'printf ready > "$1"; while [ ! -e "$2" ]; do :; done; history' \
  history-sync "$dir/sync-ready" "$dir/sync-go" > "$dir/sync-output" &
reader_pid=$!
attempt_count=0
while [ ! -e "$dir/sync-ready" ] && [ "$attempt_count" -lt 500 ]; do
  sleep 0.01
  attempt_count=$((attempt_count + 1))
done
if [ ! -e "$dir/sync-ready" ]; then
  : > "$dir/sync-go"
  wait "$reader_pid"
  exit 1
fi
KOSH_HISTORY_FILE="$dir/synchronized" "$BIN" --no-init-files -c \
  'history -s shared-event'
: > "$dir/sync-go"
wait "$reader_pid"
cat "$dir/sync-output"

printf 'one\ntwo\nthree\nfour\nfive\n' > "$dir/limit"
echo "== KOSH_HISTORY_SIZE controls the visible retained window =="
KOSH_HISTORY_FILE="$dir/limit" "$BIN" --no-init-files -c \
  'KOSH_HISTORY_SIZE=2; history; KOSH_HISTORY_SIZE=5; history'

: > "$dir/multiline"
echo "== a stored multiline event keeps its submitted shape =="
KOSH_HISTORY_FILE="$dir/multiline" "$BIN" --no-init-files -c \
  "history -s \$'line one\\nline two'; history"
