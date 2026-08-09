#!/bin/bash

TEST_SHELL_COMMAND=$1
shift
TEST_STATUS=0

for TEST_FILE in "$@"; do
  TEST_NAME=$(basename "$TEST_FILE" .sh)
  OUTPUT_DIRECTORY="$TEST_TEMP_DIRECTORY/results/build"
  mkdir -p "$OUTPUT_DIRECTORY"
  OUTPUT="$OUTPUT_DIRECTORY/$TEST_NAME.out"
  BIN="$BIN" "$TEST_SHELL_COMMAND" "$TEST_FILE" > "$OUTPUT" 2>&1
  if diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" >/dev/null 2>&1; then
    printf "\t%-64s ok\033[K\r" "build/$TEST_NAME.sh"
  else
    diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" | tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "build/$TEST_NAME.sh"
    TEST_STATUS=1
  fi
  rm -f "$OUTPUT"
done

exit "$TEST_STATUS"
