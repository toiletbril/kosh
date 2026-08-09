#!/bin/bash

test_shell=$1
shift
test_status=0

for test_file in "$@"; do
  name=$(basename "$test_file" .sh)
  output_directory="$TEST_TEMP_DIRECTORY/results/build"
  mkdir -p "$output_directory"
  output="$output_directory/$name.out"
  BIN="$BIN" "$test_shell" "$test_file" > "$output" 2>&1
  if diff $DIFF_FLAGS "expected/$name.out" "$output" >/dev/null 2>&1; then
    printf "\t%-64s ok\033[K\r" "build/$name.sh"
  else
    diff $DIFF_FLAGS "expected/$name.out" "$output" | tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "build/$name.sh"
    test_status=1
  fi
  rm -f "$output"
done

exit "$test_status"
