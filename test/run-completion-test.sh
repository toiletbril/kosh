#!/bin/bash

refill_mode=no
test_status=0
if [ "${1-}" = --refill ]; then
  refill_mode=yes
  shift
fi

test_shell=$1
shift

if [ "${IS_NONDEBUG_BUILD:-0}" = 1 ]; then
  printf "\t%-64s skipped, release binary\n" completion
  exit 0
fi

for test_file in "$@"; do
  name=$(basename "$test_file" .sh)
  if [ "$refill_mode" = yes ]; then
    output="expected/.$name.out.tmp"
  else
    output_directory="$TEST_TEMP_DIRECTORY/results/completion"
    mkdir -p "$output_directory"
    output="$output_directory/$name.out"
  fi

  BIN="$BIN" "$test_shell" "$test_file" > "$output" 2>/dev/null
  if [ "$refill_mode" = yes ]; then
    mv "$output" "expected/$name.out"
    printf "\t%-64s %s.out\n" "completion/$name.sh" "$name"
    continue
  fi

  if diff $DIFF_FLAGS "expected/$name.out" "$output" >/dev/null 2>&1; then
    printf "\t%-64s ok\033[K\r" "completion/$name.sh"
  else
    diff $DIFF_FLAGS "expected/$name.out" "$output" | tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "completion/$name.sh"
    test_status=1
  fi
  rm -f "$output"
done

exit "$test_status"
