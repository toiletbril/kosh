#!/bin/bash
REFILL_MODE=no
TEST_STATUS=0
if [ "${1-}" = --refill ]; then
  REFILL_MODE=yes
  shift
fi

TEST_SHELL_COMMAND=$1
shift

if [ "${IS_NONDEBUG_BUILD:-0}" = 1 ]; then
  printf "\t%-64s skipped, release binary\n" highlight
  exit 0
fi

for TEST_FILE in "$@"; do
  TEST_NAME=$(basename "$TEST_FILE" .sh)
  if [ "$REFILL_MODE" = yes ]; then
    OUTPUT="expected/.$TEST_NAME.out.tmp"
  else
    OUTPUT_DIRECTORY="$TEST_TEMP_DIRECTORY/results/highlight"
    mkdir -p "$OUTPUT_DIRECTORY"
    OUTPUT="$OUTPUT_DIRECTORY/$TEST_NAME.out"
  fi

  BIN="$BIN" "$TEST_SHELL_COMMAND" "$TEST_FILE" > "$OUTPUT" 2>/dev/null
  if [ "$REFILL_MODE" = yes ]; then
    mv "$OUTPUT" "expected/$TEST_NAME.out"
    printf "\t%-64s %s.out\n" "highlight/$TEST_NAME.sh" "$TEST_NAME"
    continue
  fi

  if diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" >/dev/null 2>&1; then
    printf "\t%-64s ok\033[K\r" "highlight/$TEST_NAME.sh"
  else
    diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" | \
      tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "highlight/$TEST_NAME.sh"
    TEST_STATUS=1
  fi
  rm -f "$OUTPUT"
done

exit "$TEST_STATUS"
