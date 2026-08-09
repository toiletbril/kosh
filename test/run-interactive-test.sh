#!/bin/bash

if ! command -v python3 >/dev/null 2>&1; then
  for test_file in "$@"; do
    printf "\t%-64s skipped, python3 unavailable\n" "$test_file"
  done
  exit 0
fi

output_directory="$TEST_TEMP_DIRECTORY/results/interactive"
mkdir -p "$output_directory"

for test_file in "$@"; do
  if [ "$test_file" = interactive/long_warning_window.py ] && \
    [ "${IS_NONDEBUG_BUILD:-0}" = 1 ]; then
    printf "\t%-64s skipped, release binary\n" "$test_file"
    continue
  fi

  output="$output_directory/$(basename "$test_file").out"
  if python3 "$test_file" "$BIN" > "$output" 2>&1; then
    printf "\t%-64s ok\033[K\r" "$test_file"
  else
    cat "$output"
    printf "\t%-64s FAILED :c\n" "$test_file"
    rm -f "$output"
    exit 1
  fi
  rm -f "$output"
done
