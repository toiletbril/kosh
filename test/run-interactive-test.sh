#!/bin/bash

if ! command -v python3 >/dev/null 2>&1; then
  for TEST_FILE in "$@"; do
    printf "\t%-64s skipped, python3 unavailable\n" "$TEST_FILE"
  done
  exit 0
fi

OUTPUT_DIRECTORY="$TEST_TEMP_DIRECTORY/results/interactive"
mkdir -p "$OUTPUT_DIRECTORY"

for TEST_FILE in "$@"; do
  if [ "$TEST_FILE" = interactive/long_warning_window.py ] && \
    [ "${IS_NONDEBUG_BUILD:-0}" = 1 ]; then
    printf "\t%-64s skipped, release binary\n" "$TEST_FILE"
    continue
  fi

  OUTPUT="$OUTPUT_DIRECTORY/$(basename "$TEST_FILE").out"
  if python3 "$TEST_FILE" "$BIN" > "$OUTPUT" 2>&1; then
    printf "\t%-64s ok\033[K\r" "$TEST_FILE"
  else
    cat "$OUTPUT"
    printf "\t%-64s FAILED :c\n" "$TEST_FILE"
    rm -f "$OUTPUT"
    exit 1
  fi
  rm -f "$OUTPUT"
done
