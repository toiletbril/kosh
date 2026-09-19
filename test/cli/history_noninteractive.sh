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
printf 'named base' > "$dir/named-append"
echo "== history -a separates and appends the list to a named file =="
"$BIN" -c 'history -a "$1"; echo "rc=$?"' history-test "$dir/named-append"
cat "$dir/named-append"
echo "== history -a leaves its backing file unchanged =="
"$BIN" -c 'history -a "$KOSH_HISTORY_FILE"; echo "rc=$?"; history' history-test
echo "== a hard link aliases the backing file =="
"$BIN" -c 'koshkit ln "$1" "$2"; echo "rc=$?"' history-test "$dir/hist" \
  "$dir/history-alias"
echo "== history -a leaves an aliased backing file unchanged =="
"$BIN" -c 'history -a "$1"; echo "rc=$?"; history' history-test \
  "$dir/history-alias"
printf 'named base\n' > "$dir/named-write"
echo "== history -w replaces a named file with the list =="
"$BIN" -c 'history -w "$1"; echo "rc=$?"' history-test "$dir/named-write"
cat "$dir/named-write"
echo "== history -r on a missing file errors =="
"$BIN" -c 'history -r "$1"; echo "rc=$?"' history-test \
  "$dir/no-such-history" 2>/dev/null

printf '\001' > "$dir/invalid"
echo "== history -r rejects invalid data without changing the list =="
"$BIN" -c 'history -r "$1"; echo "rc=$?"; history' history-test \
  "$dir/invalid" 2>/dev/null

printf '\377\n' > "$dir/invalid-utf8"
echo "== history -r rejects malformed UTF-8 without changing the list =="
"$BIN" -c 'history -r "$1"; echo "rc=$?"; history' history-test \
  "$dir/invalid-utf8" 2>/dev/null

printf 'tail' > "$dir/unterminated"
printf 'next\n' > "$dir/next"
echo "== history -r separates an unterminated backing record =="
KOSH_HISTORY_FILE="$dir/unterminated" "$BIN" --no-init-files -c \
  'history -r "$1"; history' history-test "$dir/next"
printf 'next' > "$dir/next"
echo "== history -r terminates an unterminated imported record =="
KOSH_HISTORY_FILE="$dir/unterminated-import-backing" "$BIN" --no-init-files -c \
  'history -r "$1"; history' history-test "$dir/next"

: > "$dir/empty"
echo "== an empty import creates a missing backing file =="
KOSH_HISTORY_FILE="$dir/empty-backing" "$BIN" --no-init-files -c \
  'history -r "$1"; echo "rc=$?"' history-test "$dir/empty"

echo "== history stores into a missing backing file =="
KOSH_HISTORY_FILE="$dir/missing-backing" "$BIN" --no-init-files -c \
  'history -s created; echo "rc=$?"; history'
KOSH_HISTORY_FILE="$dir/missing-backing" "$BIN" --no-init-files -c 'history'

printf '\377\n' > "$dir/high-byte"
echo "== history accepts high bytes as file data =="
KOSH_HISTORY_FILE="$dir/high-byte" "$BIN" --no-init-files -c \
  'history | koshkit wc -l'

printf '\303\251\n\344\275\240\n\360\237\230\200\n' > "$dir/valid-utf8"
echo "== history accepts valid UTF-8 as imported file data =="
KOSH_HISTORY_FILE="$dir/valid-utf8-backing" "$BIN" --no-init-files -c \
  'history -r "$1"; echo "rc=$?"; history | koshkit wc -l' history-test \
  "$dir/valid-utf8"

for invalid_bytes in '\200' '\300\200' '\340\200\200' '\355\240\200' \
  '\364\220\200\200'; do
  printf "$invalid_bytes" > "$dir/invalid-utf8"
  printf '== history rejects malformed UTF-8 imported bytes %s ==\n' \
    "$invalid_bytes"
  "$BIN" -c 'history -r "$1"; echo "rc=$?"' history-test \
    "$dir/invalid-utf8" 2>/dev/null
done
printf '\303' > "$dir/invalid-utf8"
echo "== history rejects a truncated UTF-8 imported sequence =="
"$BIN" -c 'history -r "$1"; echo "rc=$?"' history-test \
  "$dir/invalid-utf8" 2>/dev/null

: > "$dir/oversized"
byte_count=0
while [ "$byte_count" -lt 4096 ]; do
  printf x >> "$dir/oversized"
  byte_count=$((byte_count + 1))
done
printf '\n' >> "$dir/oversized"
echo "== an oversized decoded record is reported =="
KOSH_HISTORY_FILE="$dir/oversized" "$BIN" --no-init-files -c 'history' 2>&1 | \
  grep -c "cannot read history at "
KOSH_HISTORY_FILE="$dir/oversized" "$BIN" --no-init-files -c \
  'history; echo "rc=$?"' 2>/dev/null

: > "$dir/concurrent"
echo "== concurrent history stores preserve both records =="
KOSH_HISTORY_FILE="$dir/concurrent" "$BIN" --no-init-files -c \
  'history -s first' &
first_pid=$!
KOSH_HISTORY_FILE="$dir/concurrent" "$BIN" --no-init-files -c \
  'history -s second' &
second_pid=$!
wait "$first_pid"
first_status=$?
wait "$second_pid"
second_status=$?
printf 'rc=%s,%s\n' "$first_status" "$second_status"
KOSH_HISTORY_FILE="$dir/concurrent" "$BIN" --no-init-files -c \
  'history | koshkit wc -l'

: > "$dir/duplicate"
echo "== consecutive duplicate stores succeed and retain one record =="
KOSH_HISTORY_FILE="$dir/duplicate" "$BIN" --no-init-files -c \
  'history -s repeated; first_status=$?; history -s repeated; \
second_status=$?; printf "rc=%s,%s\n" "$first_status" "$second_status"; history'

: > "$dir/zero-limit"
echo "== a zero KOSH_HISTORY_SIZE retains and stores nothing =="
KOSH_HISTORY_FILE="$dir/zero-limit" "$BIN" --no-init-files -c \
  'KOSH_HISTORY_SIZE=0; history -s x; single_status=$?; \
history -s repeated; first_status=$?; history -s repeated; second_status=$?; \
printf "rc=%s,%s,%s\n" "$single_status" "$first_status" "$second_status"; \
history; KOSH_HISTORY_SIZE=5; history; \
koshkit wc -l < "$KOSH_HISTORY_FILE"'

: > "$dir/repeated"
echo "== nonconsecutive duplicate stores remain separate records =="
KOSH_HISTORY_FILE="$dir/repeated" "$BIN" --no-init-files -c \
  'history -s alpha; history -s beta; history -s alpha; history'

printf 'first\n' > "$dir/equal-size"
printf second > "$dir/equal-size-replacement"
echo "== an equal size replacement refreshes append state =="
KOSH_HISTORY_FILE="$dir/equal-size" "$BIN" --no-init-files -c \
  'history >/dev/null; koshkit mv "$1" "$KOSH_HISTORY_FILE"; \
history -s third; history' history-test "$dir/equal-size-replacement"

printf 'mode one\n' > "$dir/mode-change"
echo "== a mode change on the backing file keeps the store readable =="
KOSH_HISTORY_FILE="$dir/mode-change" "$BIN" --no-init-files -c \
  'history >/dev/null; koshkit chmod 700 "$KOSH_HISTORY_FILE"; \
echo "rc=$?"; history -s modetwo; history'

: > "$dir/rewrite"
echo "== history deletion rewrites the no editor store =="
KOSH_HISTORY_FILE="$dir/rewrite" "$BIN" --no-init-files -c \
  'history -s first; history -s second; history -d 1; history'

printf '\377\nsecond\n' > "$dir/high-byte-rewrite"
echo "== history deletion preserves legacy high-byte records =="
KOSH_HISTORY_FILE="$dir/high-byte-rewrite" "$BIN" --no-init-files -c \
  'history -d 2; echo "rc=$?"; history | koshkit wc -l'

printf 'same\n' > "$dir/replaced-receipt"
printf 'same\npeer\n' > "$dir/replaced-receipt-next"
echo "== deletion rejects a startup receipt from a replaced file =="
KOSH_HISTORY_FILE="$dir/replaced-receipt" "$BIN" --no-init-files -c \
  'history >/dev/null; koshkit mv "$1" "$KOSH_HISTORY_FILE"; \
history -s local; history -d 1; echo "rc=$?"; history; \
echo durable; cat "$KOSH_HISTORY_FILE"' history-test \
  "$dir/replaced-receipt-next" 2>/dev/null

printf 'same\n' > "$dir/replaced-fc-receipt"
printf 'same\npeer\n' > "$dir/replaced-fc-receipt-next"
echo "== fc rejects a startup receipt from a replaced file =="
KOSH_HISTORY_FILE="$dir/replaced-fc-receipt" "$BIN" --no-init-files -c \
  'history >/dev/null; koshkit mv "$1" "$KOSH_HISTORY_FILE"; \
history -s same; fc -s 1; echo "rc=$?"; history; \
echo durable; cat "$KOSH_HISTORY_FILE"' history-test \
  "$dir/replaced-fc-receipt-next" 2>/dev/null

printf same > "$dir/replaced-tail-receipt"
printf 'same\npeer\n' > "$dir/replaced-tail-receipt-next"
echo "== a promoted startup tail keeps its original receipt =="
KOSH_HISTORY_FILE="$dir/replaced-tail-receipt" "$BIN" --no-init-files -c \
  'history >/dev/null; koshkit mv "$1" "$KOSH_HISTORY_FILE"; \
history -s local; history -d 1; echo "rc=$?"; history; \
echo durable; cat "$KOSH_HISTORY_FILE"' history-test \
  "$dir/replaced-tail-receipt-next" 2>/dev/null

: > "$dir/rewrite-range"
echo "== a range deletion renumbers before the next store =="
KOSH_HISTORY_FILE="$dir/rewrite-range" "$BIN" --no-init-files -c \
  'history -s first; history -s second; history -s third; history -d 1-2; \
delete_status=$?; history -s fourth; store_status=$?; \
printf "rc=%s,%s\n" "$delete_status" "$store_status"; history'

printf 'valid\n' > "$dir/invalid-store"
echo "== invalid stored bytes leave existing history intact =="
KOSH_HISTORY_FILE="$dir/invalid-store" "$BIN" --no-init-files -c \
  "history -s \$'bad\\evalue'; printf 'rc=%s\\n' \"\$?\"; history" \
  2>/dev/null

for invalid_bytes in '\377' '\200' '\303' '\300\200' '\340\200\200' \
  '\355\240\200' '\364\220\200\200'; do
  printf 'valid\n' > "$dir/invalid-utf8-store"
  printf '== malformed UTF-8 stored bytes %s leave existing history intact ==\n' \
    "$invalid_bytes"
  KOSH_HISTORY_FILE="$dir/invalid-utf8-store" "$BIN" --no-init-files -c \
    "history -s \$'bad${invalid_bytes}value'; printf 'rc=%s\\n' \"\$?\"; history" \
    2>/dev/null
done

printf 'startup-event\n' > "$dir/synchronized"
echo "== running shells keep their startup history branch private =="
KOSH_HISTORY_FILE="$dir/synchronized" "$BIN" --no-init-files -c \
  'history >/dev/null; printf ready > "$1"; attempt_count=0; \
while [ ! -e "$2" ] && [ "$attempt_count" -lt 500 ]; do \
koshkit sleep 0.01; attempt_count=$((attempt_count + 1)); done; \
[ -e "$2" ] || exit 1; history; history -s local-event; history' \
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
  echo "history synchronization reader timed out" >&2
  exit 1
fi
KOSH_HISTORY_FILE="$dir/synchronized" "$BIN" --no-init-files -c \
  'history -s shared-event'
: > "$dir/sync-go"
wait "$reader_pid"
cat "$dir/sync-output"
echo "== a new shell starts with every durable history branch =="
KOSH_HISTORY_FILE="$dir/synchronized" "$BIN" --no-init-files -c 'history'

printf 'one\ntwo\nthree\nfour\nfive\n' > "$dir/limit"
echo "== KOSH_HISTORY_SIZE controls the visible retained window =="
KOSH_HISTORY_FILE="$dir/limit" "$BIN" --no-init-files -c \
  'KOSH_HISTORY_SIZE=2; history; KOSH_HISTORY_SIZE=5; history; history -s six; \
history'

printf 'good\n\001bad\n' > "$dir/damaged"
echo "== a bare listing reports a damaged backing file =="
KOSH_HISTORY_FILE="$dir/damaged" "$BIN" --no-init-files -c 'history' 2>&1 | \
  grep -c "cannot read history at .*: the file contains invalid data"
KOSH_HISTORY_FILE="$dir/damaged" "$BIN" --no-init-files -c 'history' 2>/dev/null
echo "rc=$?"

echo "== an operandless read names the resolved backing file =="
KOSH_HISTORY_FILE="$dir/no-such-backing" "$BIN" --no-init-files -c \
  'history -r' 2>&1 | grep -c "cannot read history at "
mkdir "$dir/backing-directory"
KOSH_HISTORY_FILE="$dir/backing-directory" "$BIN" --no-init-files -c \
  'history -r' 2>&1 | grep -c "cannot read history at "

: > "$dir/multiline"
echo "== a stored multiline event keeps its submitted shape =="
KOSH_HISTORY_FILE="$dir/multiline" "$BIN" --no-init-files -c \
  "history -s \$'line one\\nline two'; history"

printf 'named base\n' > "$dir/repeated-append"
echo "== a repeated named append stores only the events not written yet =="
KOSH_HISTORY_FILE="$dir/repeated-append-backing" "$BIN" --no-init-files -c \
  'history -s one; history -a "$1"; history -s two; history -a "$1"; \
echo "rc=$?"' history-test "$dir/repeated-append"
cat "$dir/repeated-append"

printf 'first target\n' > "$dir/append-one"
printf 'second target\n' > "$dir/append-two"
echo "== appends to different files each receive the retained list =="
KOSH_HISTORY_FILE="$dir/two-target-backing" "$BIN" --no-init-files -c \
  'history -s alpha; history -a "$1"; history -a "$2"; echo "rc=$?"' \
  history-test "$dir/append-one" "$dir/append-two"
cat "$dir/append-one"
cat "$dir/append-two"

echo "== a named write to a directory fails and leaves the directory in place =="
KOSH_HISTORY_FILE="$dir/write-fail-backing" "$BIN" --no-init-files -c \
  'history -s kept; history -w "$1"; echo "rc=$?"; \
if [ -d "$1" ]; then echo "directory intact"; fi; history' history-test \
  "$dir/backing-directory" 2>/dev/null
KOSH_HISTORY_FILE="$dir/write-fail-backing" "$BIN" --no-init-files -c \
  'history -w "$1"' history-test "$dir/backing-directory" 2>&1 | \
  grep -c "cannot write history to '"

echo "== a named append to a directory fails =="
KOSH_HISTORY_FILE="$dir/append-fail-backing" "$BIN" --no-init-files -c \
  'history -s kept; history -a "$1"; echo "rc=$?"' history-test \
  "$dir/backing-directory" 2>/dev/null

echo "== an operandless write names the resolved backing file =="
KOSH_HISTORY_FILE="$dir/backing-directory" "$BIN" --no-init-files -c \
  'history -w; echo "rc=$?"' 2>&1 | grep -c "cannot write history at "

printf 'append base\n' > "$dir/append-limit-target"
echo "== a named append is bounded by KOSH_HISTORY_SIZE =="
KOSH_HISTORY_FILE="$dir/append-limit" "$BIN" --no-init-files -c \
  'KOSH_HISTORY_SIZE=2; history -s one; history -s two; history -s three; \
history -a "$1"; echo "rc=$?"' history-test "$dir/append-limit-target"
cat "$dir/append-limit-target"

printf 'import one\nimport two\n' > "$dir/import-source"
echo "== a repeated new read skips the bytes it already took =="
KOSH_HISTORY_FILE="$dir/import-new-backing" "$BIN" --no-init-files -c \
  'history -n "$1"; history -n "$1"; history' history-test "$dir/import-source"
echo "== a repeated read takes the whole file again =="
KOSH_HISTORY_FILE="$dir/import-read-backing" "$BIN" --no-init-files -c \
  'history -r "$1"; history -r "$1"; history' history-test "$dir/import-source"

printf 'one\ntwo\nthree\nfour\nfive\n' > "$dir/write-trim"
echo "== an operandless write trims the backing file to the retained window =="
KOSH_HISTORY_FILE="$dir/write-trim" "$BIN" --no-init-files -c \
  'KOSH_HISTORY_SIZE=2; history -w; echo "rc=$?"'
cat "$dir/write-trim"

printf 'one\ntwo\nthree\nfour\nfive\n' > "$dir/write-limit"
echo "== a named write is bounded by KOSH_HISTORY_SIZE =="
KOSH_HISTORY_FILE="$dir/write-limit" "$BIN" --no-init-files -c \
  'KOSH_HISTORY_SIZE=2; history -w "$1"; echo "rc=$?"' history-test \
  "$dir/write-limit-target"
cat "$dir/write-limit-target"

: > "$dir/carriage-return"
echo "== a stored carriage return stays inside the event =="
KOSH_HISTORY_FILE="$dir/carriage-return" "$BIN" --no-init-files -c \
  "history -s \$'a\\rb'; printf 'rc=%s\\n' \"\$?\"; listed=\$(history); \
printf '%s\\n' \"\${listed//\$'\\r'/R}\""

printf 'crlf one\r\ncrlf two\r\n' > "$dir/crlf"
echo "== a CRLF backing file drops only the record terminator =="
KOSH_HISTORY_FILE="$dir/crlf" "$BIN" --no-init-files -c \
  "listed=\$(history); printf '%s\\n' \"\${listed//\$'\\r'/R}\""

printf 'race one\nrace two\nrace three\n' > "$dir/race"
echo "== a concurrent deletion and store leave a readable store =="
KOSH_HISTORY_FILE="$dir/race" "$BIN" --no-init-files -c 'history -d 2' &
delete_pid=$!
KOSH_HISTORY_FILE="$dir/race" "$BIN" --no-init-files -c 'history -s racefour' &
store_pid=$!
wait "$delete_pid"
delete_status=$?
wait "$store_pid"
store_status=$?
printf 'rc=%s,%s\n' "$delete_status" "$store_status"
KOSH_HISTORY_FILE="$dir/race" "$BIN" --no-init-files -c \
  'history >/dev/null; echo "rc=$?"'

printf 'grow one\n' > "$dir/grow"
echo "== a store after an external append keeps its private branch =="
KOSH_HISTORY_FILE="$dir/grow" "$BIN" --no-init-files -c \
  'history >/dev/null; printf "grow two\n" >> "$KOSH_HISTORY_FILE"; \
history -s growthree; history'
echo "== a new shell sees every externally appended record =="
KOSH_HISTORY_FILE="$dir/grow" "$BIN" --no-init-files -c 'history'

echo "== a missing startup file freezes an empty private branch =="
KOSH_HISTORY_FILE="$dir/missing-startup" "$BIN" --no-init-files -c \
  'history >/dev/null; printf ready > "$1"; attempt_count=0; \
while [ ! -e "$2" ] && [ "$attempt_count" -lt 500 ]; do \
koshkit sleep 0.01; attempt_count=$((attempt_count + 1)); done; \
[ -e "$2" ] || exit 1; history -s local-missing; history' history-test \
  "$dir/missing-ready" "$dir/missing-go" > "$dir/missing-output" &
missing_reader_pid=$!
attempt_count=0
while [ ! -e "$dir/missing-ready" ] && [ "$attempt_count" -lt 500 ]; do
  "$BIN" --no-init-files -c 'koshkit sleep 0.01'
  attempt_count=$((attempt_count + 1))
done
if [ ! -e "$dir/missing-ready" ]; then
  kill "$missing_reader_pid" 2>/dev/null
  wait "$missing_reader_pid" 2>/dev/null
  echo "missing reader did not become ready" >&2
  exit 1
fi
printf 'peer-missing\n' > "$dir/missing-startup"
: > "$dir/missing-go"
wait "$missing_reader_pid"
cat "$dir/missing-output"
echo "== a new shell sees both missing-file branches =="
KOSH_HISTORY_FILE="$dir/missing-startup" "$BIN" --no-init-files -c 'history'

printf 'tail one\n' > "$dir/unterminated-peer"
echo "== a peer unterminated tail stays separate from a local append =="
KOSH_HISTORY_FILE="$dir/unterminated-peer" "$BIN" --no-init-files -c \
  'history >/dev/null; printf peer-tail >> "$KOSH_HISTORY_FILE"; \
history -s local-tail; history'
echo "== a new shell reads separate peer and local tail records =="
KOSH_HISTORY_FILE="$dir/unterminated-peer" "$BIN" --no-init-files -c \
  'history'

printf 'delete base\n' > "$dir/divergent-delete-local"
echo "== deleting a local event preserves an unseen peer =="
KOSH_HISTORY_FILE="$dir/divergent-delete-local" "$BIN" --no-init-files -c \
  'history >/dev/null; printf "delete peer\n" >> "$KOSH_HISTORY_FILE"; \
history -s "delete local"; history -d 2; history'
echo "== a fresh shell sees the preserved peer after local deletion =="
KOSH_HISTORY_FILE="$dir/divergent-delete-local" "$BIN" --no-init-files -c \
  'history'

printf 'delete startup\n' > "$dir/divergent-delete-startup"
echo "== deleting a startup event keeps the private local event =="
KOSH_HISTORY_FILE="$dir/divergent-delete-startup" "$BIN" --no-init-files -c \
  'history >/dev/null; printf "startup peer\n" >> "$KOSH_HISTORY_FILE"; \
history -s "startup local"; history -d 1; history'
echo "== a fresh shell sees the peer and local event after startup deletion =="
KOSH_HISTORY_FILE="$dir/divergent-delete-startup" "$BIN" --no-init-files -c \
  'history'

printf 'append base\n' > "$dir/divergent-append"
echo "== operandless history append leaves the private branch unchanged =="
KOSH_HISTORY_FILE="$dir/divergent-append" "$BIN" --no-init-files -c \
  'history >/dev/null; printf "append peer\n" >> "$KOSH_HISTORY_FILE"; \
history -s "append local"; history -a; history'
echo "== a fresh shell sees every append branch =="
KOSH_HISTORY_FILE="$dir/divergent-append" "$BIN" --no-init-files -c 'history'

printf 'write base\n' > "$dir/divergent-write"
echo "== operandless history write keeps only the private branch =="
KOSH_HISTORY_FILE="$dir/divergent-write" "$BIN" --no-init-files -c \
  'history >/dev/null; printf "write peer\n" >> "$KOSH_HISTORY_FILE"; \
history -s "write local"; history -w; history'
echo "== a fresh shell sees the private branch written without peers =="
KOSH_HISTORY_FILE="$dir/divergent-write" "$BIN" --no-init-files -c 'history'

printf 'old symlink\n' > "$dir/write-symlink-target"
"$BIN" --no-init-files -c 'koshkit ln -s "$1" "$2"' history-test \
  "$dir/write-symlink-target" "$dir/write-symlink"
echo "== a named write preserves a symlink target =="
KOSH_HISTORY_FILE="$dir/write-symlink-backing" "$BIN" --no-init-files -c \
  'history -s symlinked; history -w "$1"; write_status=$?; \
if [ -L "$1" ]; then link_status=0; else link_status=1; fi; \
printf "rc=%s link=%s\n" "$write_status" "$link_status"' history-test \
  "$dir/write-symlink"
cat "$dir/write-symlink-target"

printf 'old hardlink\n' > "$dir/write-hardlink-target"
"$BIN" --no-init-files -c 'koshkit ln "$1" "$2"' history-test \
  "$dir/write-hardlink-target" "$dir/write-hardlink"
echo "== a named write preserves hardlink identity =="
KOSH_HISTORY_FILE="$dir/write-hardlink-backing" "$BIN" --no-init-files -c \
  'history -s hardlinked; history -w "$1"; write_status=$?; \
if [ "$1" -ef "$2" ]; then same_status=0; else same_status=1; fi; \
printf "rc=%s same=%s\n" "$write_status" "$same_status"' history-test \
  "$dir/write-hardlink" "$dir/write-hardlink-target"
cat "$dir/write-hardlink-target"

printf 'old mode\n' > "$dir/write-mode"
"$BIN" --no-init-files -c 'koshkit chmod 700 "$1"' history-test \
  "$dir/write-mode"
echo "== a named write preserves the target mode =="
KOSH_HISTORY_FILE="$dir/write-mode-backing" "$BIN" --no-init-files -c \
  'history -s modekept; history -w "$1"; write_status=$?; \
if [ -x "$1" ]; then mode_status=0; else mode_status=1; fi; \
printf "rc=%s mode=%s\n" "$write_status" "$mode_status"' history-test \
  "$dir/write-mode"
cat "$dir/write-mode"

printf 'new one\nnew two\n' > "$dir/import-alias-source"
echo "== new reads share state across equivalent path spellings =="
KOSH_HISTORY_FILE="$dir/import-alias-backing" "$BIN" --no-init-files -c \
  'history -n "$1"; history -n "$2"; history' history-test \
  "$dir/import-alias-source" "$dir/./import-alias-source"

printf 'old one\nold two\n' > "$dir/import-rewrite-source"
echo "== a rewritten new-read source restarts from the beginning =="
KOSH_HISTORY_FILE="$dir/import-rewrite-backing" "$BIN" --no-init-files -c \
  'history -n "$1"; printf "replacement\n" > "$1"; history -n "$1"; history' \
  history-test "$dir/import-rewrite-source"

printf 'before replacement\n' > "$dir/import-replaced-source"
printf 'after replacement\n' > "$dir/import-source-replacement"
echo "== a replaced new-read source restarts from the beginning =="
KOSH_HISTORY_FILE="$dir/import-replaced-backing" "$BIN" --no-init-files -c \
  'history -n "$1"; koshkit mv "$2" "$1"; history -n "$1"; history' \
  history-test "$dir/import-replaced-source" "$dir/import-source-replacement"

printf 'append base\n' > "$dir/append-alias-target"
echo "== named appends share state across equivalent path spellings =="
KOSH_HISTORY_FILE="$dir/append-alias-backing" "$BIN" --no-init-files -c \
  'history -s aliasone; history -a "$1"; history -s aliastwo; \
history -a "$2"; echo "rc=$?"' history-test "$dir/append-alias-target" \
  "$dir/./append-alias-target"
cat "$dir/append-alias-target"

printf 'one\ntwo\nthree\n' > "$dir/backing-write"
echo "== a named write trims its backing file =="
KOSH_HISTORY_FILE="$dir/backing-write" "$BIN" --no-init-files -c \
  'KOSH_HISTORY_SIZE=2; history -w "$KOSH_HISTORY_FILE"; echo "rc=$?"'
cat "$dir/backing-write"

: > "$dir/concurrent-named-append"
echo "== concurrent named appends keep complete records =="
KOSH_HISTORY_FILE="$dir/named-append-one" "$BIN" --no-init-files -c \
  'history -s appendone; history -a "$1"' history-test \
  "$dir/concurrent-named-append" &
first_pid=$!
KOSH_HISTORY_FILE="$dir/named-append-two" "$BIN" --no-init-files -c \
  'history -s appendtwo; history -a "$1"' history-test \
  "$dir/concurrent-named-append" &
second_pid=$!
wait "$first_pid"
first_status=$?
wait "$second_pid"
second_status=$?
printf 'rc=%s,%s\n' "$first_status" "$second_status"
KOSH_HISTORY_FILE="$dir/concurrent-named-append" "$BIN" --no-init-files -c \
  'history | koshkit wc -l'

printf 'old\n' > "$dir/concurrent-named-write"
echo "== a concurrent named write and append leave readable history =="
KOSH_HISTORY_FILE="$dir/named-write-backing" "$BIN" --no-init-files -c \
  'history -s written; history -w "$1"' history-test \
  "$dir/concurrent-named-write" &
write_pid=$!
KOSH_HISTORY_FILE="$dir/named-append-backing" "$BIN" --no-init-files -c \
  'history -s appended; history -a "$1"' history-test \
  "$dir/concurrent-named-write" &
append_pid=$!
wait "$write_pid"
write_status=$?
wait "$append_pid"
append_status=$?
printf 'rc=%s,%s\n' "$write_status" "$append_status"
KOSH_HISTORY_FILE="$dir/concurrent-named-write" "$BIN" --no-init-files -c \
  'history >/dev/null; echo "rc=$?"'

printf '\001bad\n' > "$dir/exact-failure"
echo "== a history read reports its owned failure cause =="
KOSH_HISTORY_FILE="$dir/exact-failure" "$BIN" --no-init-files -c \
  'koshkit cat "$1" >/dev/null 2>&1; history -r' history-test \
  "$dir/no-such-file" 2>&1 | \
  grep -c "cannot read history at .*: the file contains invalid data"
