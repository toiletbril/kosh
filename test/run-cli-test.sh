#!/bin/bash
. ./runner-status.sh

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
  arena_lifetime|command_substitution_strategy|fg_terminal_handoff|\
    toiletline_allocation)
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

  SHOULD_BOUND_CLI_TEST=no
  case $TEST_NAME in
  command_substitution_interrupt|fg_terminal_handoff|history_behavior|\
    history_noninteractive|read_behavior|language_server|koshkit_fuser|\
    koshkit_evil_regressions|koshkit_eviliso_loopback|koshkit_process_tools|\
    koshkit_timeout|\
    transaction_lock_lifetime|\
    subshell_spawn_state|wait_on_stopped_job|\
    trap_action_return|trap_action_loop_control|\
    trap_during_blocking_builtin|interrupt_during_blocking_open)
    SHOULD_BOUND_CLI_TEST=yes
    ;;
  esac
  if [ -n "${CLI_TEST_TIMEOUT_SECONDS-}" ]; then
    SHOULD_BOUND_CLI_TEST=yes
  fi

  if [ "$SHOULD_BOUND_CLI_TEST" = yes ]; then
    GOLDEN_TIMEOUT_SECONDS=60
    if [ "$TEST_NAME" = history_behavior ] || [ "$TEST_NAME" = koshkit_timeout ]; then
      GOLDEN_TIMEOUT_SECONDS=120
    fi
    CLI_TEST_TIMEOUT_SECONDS=${CLI_TEST_TIMEOUT_SECONDS:-$GOLDEN_TIMEOUT_SECONDS} \
      BIN="$BIN" "$TEST_SHELL_COMMAND" ./run-bounded-cli-golden.sh \
      "$TEST_FILE" > "$OUTPUT" 2>&1
    DRIVER_STATUS=$?
  else
    BIN="$BIN" "$TEST_SHELL_COMMAND" "$TEST_FILE" > "$OUTPUT" 2>&1
    DRIVER_STATUS=$?
  fi

  if is_driver_status_harness_failure "$DRIVER_STATUS" "$REFILL_MODE"; then
    command cat "$OUTPUT"
    printf "\t%-64s harness failure, status %s\n" \
      "cli/$TEST_NAME.sh" "$DRIVER_STATUS"
    rm -f "$OUTPUT"
    if [ "$TEST_STATUS" -eq 0 ]; then
      TEST_STATUS=$DRIVER_STATUS
    fi
    continue
  fi

  if [ "$REFILL_MODE" = yes ]; then
    mv "$OUTPUT" "expected/$TEST_NAME.out"
    printf "\t%-64s cli/%s.out\n" "cli/$TEST_NAME.sh" "$TEST_NAME"
    continue
  fi

  if diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" >/dev/null 2>&1; then
    printf "\t%-64s ok\033[K\r" "cli/$TEST_NAME.sh"
  else
    set_golden_failure_file "cli-$TEST_NAME"
    diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" | \
      tee -a "$GOLDEN_FAILURE_FILE"
    printf "\t%-64s FAILED :c\n" "cli/$TEST_NAME.sh"
    if [ "$TEST_STATUS" -eq 0 ]; then
      TEST_STATUS=1
    fi
  fi
  rm -f "$OUTPUT"
done

exit "$TEST_STATUS"
