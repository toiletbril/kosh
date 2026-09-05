#!/bin/sh
#
#    This file is a part of the Koshka shell, (c) toiletbril, 2026
#    See the top-level LICENSE file for the licensing information.
#
# This script verifies time keyword and builtin formatting, resident memory
# reporting, runtime mood changes, and explicit TIMEFORMAT values.

unset KOSH_FLAGS

count_lines()
{
  printf '%s\n' "$2" | grep -c "$1" || true
}

report=$("$BIN" --no-init-files --mood bash -c \
  'time "$1" --no-init-files -c :; set --mood kosh; time "$1" --no-init-files -c :' \
  time-test "$BIN" 2>&1)
echo "runtime-mood-rss=$(count_lines '^  rss    ' "$report")"

report=$("$BIN" --no-init-files --no-diagnostics -c \
  'TIMEFORMAT=""; time -R "$1" --no-init-files -c :' \
  time-test "$BIN" 2>&1)
echo "kosh-rss=$(count_lines '^  rss    ' "$report")"

report=$("$BIN" --no-init-files --no-diagnostics -c \
  'TIMEFORMAT=""; time -R true' 2>&1)
echo "zero-rss=$(count_lines '^  rss    ' "$report")"

report=$("$BIN" --no-init-files --no-diagnostics -c \
  'TIMEFORMAT=""; builtin time -R "$1" --no-init-files -c :' \
  time-test "$BIN" 2>&1)
echo "builtin-rss=$(count_lines '^  rss    ' "$report")"

report=$("$BIN" --no-init-files --no-diagnostics -c \
  'time -p -R "$1" --no-init-files -c :' \
  time-test "$BIN" 2>&1)
echo "posix-real=$(count_lines '^real ' "$report") posix-rss=$(count_lines '^  rss    ' "$report")"

report=$("$BIN" --no-init-files --mood bash -c \
  'TIMEFORMAT=custom; time -R "$1" --no-init-files -c :' \
  time-test "$BIN" 2>&1)
echo "bash-custom=$(count_lines '^custom$' "$report") bash-rss=$(count_lines '^  rss    ' "$report")"
