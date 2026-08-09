#!/bin/bash
REFILL_MODE=no
TEST_STATUS=0
if [ "${1-}" = --refill ]; then
  REFILL_MODE=yes
  shift
fi

TEST_SHELL_COMMAND=$1
shift

for TEST_FILE in "$@"; do
  TEST_NAME=$(basename "$TEST_FILE" .sh)
  case $TEST_NAME in
  command_substitution_strategy|fg_terminal_handoff)
    if [ "${IS_NONDEBUG_BUILD:-0}" = 1 ]; then
      printf "\t%-64s skipped, release binary\n" "cli/$TEST_NAME.sh"
      continue
    fi
    ;;
  esac
  if [ "$REFILL_MODE" = yes ]; then
    OUTPUT="expected/.$TEST_NAME.out.tmp"
  else
    OUTPUT_DIRECTORY="$TEST_TEMP_DIRECTORY/results/cli"
    mkdir -p "$OUTPUT_DIRECTORY"
    OUTPUT="$OUTPUT_DIRECTORY/$TEST_NAME.out"
  fi

  case $TEST_NAME in
  command_substitution_interrupt|fg_terminal_handoff|history_behavior|read_behavior|\
    shitbox_timeout|transaction_lock_lifetime|wait_on_stopped_job)
    GOLDEN_TIMEOUT_SECONDS=60
    if [ "$TEST_NAME" = history_behavior ] || [ "$TEST_NAME" = shitbox_timeout ]; then
      GOLDEN_TIMEOUT_SECONDS=120
    fi
    CLI_TEST_TIMEOUT_SECONDS=${CLI_TEST_TIMEOUT_SECONDS:-$GOLDEN_TIMEOUT_SECONDS} \
      BIN="$BIN" "$TEST_SHELL_COMMAND" ./run-bounded-cli-golden.sh \
      "$TEST_FILE" > "$OUTPUT" 2>&1
    DRIVER_STATUS=$?
    if [ "$REFILL_MODE" = yes ] && [ "$DRIVER_STATUS" -ne 0 ]; then
      rm -f "$OUTPUT"
      exit "$DRIVER_STATUS"
    fi
    if [ "$REFILL_MODE" = no ] && [ "$DRIVER_STATUS" -ne 0 ]; then
      printf 'golden exited with status %s\n' "$DRIVER_STATUS" >> "$OUTPUT"
    fi
    ;;
  *)
    BIN="$BIN" "$TEST_SHELL_COMMAND" "$TEST_FILE" > "$OUTPUT" 2>&1
    ;;
  esac
  if [ "$REFILL_MODE" = yes ]; then
    mv "$OUTPUT" "expected/$TEST_NAME.out"
    printf "\t%-64s cli/%s.out\n" "cli/$TEST_NAME.sh" "$TEST_NAME"
    continue
  fi

  if diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" >/dev/null 2>&1; then
    printf "\t%-64s ok\033[K\r" "cli/$TEST_NAME.sh"
  else
    diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" | tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "cli/$TEST_NAME.sh"
    TEST_STATUS=1
  fi
  rm -f "$OUTPUT"
done

exit "$TEST_STATUS"
