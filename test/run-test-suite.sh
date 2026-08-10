#!/bin/bash

SUITE_NAME=${1:-all}
WORKER_PIDS=
WORKER_COUNT=0
WORKER_STATUS=0

word_is_listed()
{
  WORD=$1
  WORDS=$2

  case " $WORDS " in
  *" $WORD "*) return 0 ;;
  *) return 1 ;;
  esac
}

ACTIVE_TEST_NAMES=
for TEST_FILE in kosh/*.kosh; do
  TEST_NAME=${TEST_FILE#kosh/}
  TEST_NAME=${TEST_NAME%.kosh}
  if word_is_listed "$TEST_NAME" "$SKIPPED_TEST_NAMES"; then
    continue
  fi
  ACTIVE_TEST_NAMES="$ACTIVE_TEST_NAMES $TEST_NAME"
done

SERIAL_CLI_CANDIDATES="cli/set_option_state.sh cli/read_behavior.sh"
PARALLEL_CLI_INPUT=
SERIAL_CLI_INPUT=
for TEST_FILE in cli/*.sh; do
  if word_is_listed "$TEST_FILE" "$SKIPPED_CLI_INPUT"; then
    continue
  fi
  if [ -n "${SKIP_CLI_ASSIMILATE-}" ] && \
    [ "$TEST_FILE" = cli/assimilate.sh ]; then
    continue
  fi
  if [ -n "${SKIP_CLI_BASH_XTRACEFD-}" ] && \
    [ "$TEST_FILE" = cli/bash_xtracefd.sh ]; then
    continue
  fi
  if word_is_listed "$TEST_FILE" "$SERIAL_CLI_CANDIDATES"; then
    SERIAL_CLI_INPUT="$SERIAL_CLI_INPUT $TEST_FILE"
  else
    PARALLEL_CLI_INPUT="$PARALLEL_CLI_INPUT $TEST_FILE"
  fi
done

ACTIVE_BUILD_INPUT=
for TEST_FILE in build/*.sh; do
  if word_is_listed "$TEST_FILE" "$SKIPPED_BUILD_INPUT"; then
    continue
  fi
  ACTIVE_BUILD_INPUT="$ACTIVE_BUILD_INPUT $TEST_FILE"
done

SERIAL_COMPLETION_CANDIDATES=completion/editor_append_hot_path.sh
PARALLEL_COMPLETION_INPUT=
SERIAL_COMPLETION_INPUT=
for TEST_FILE in completion/*.sh; do
  if word_is_listed "$TEST_FILE" "$SKIPPED_COMPLETION_INPUT" || \
    word_is_listed "$TEST_FILE" "$UNREPRESENTABLE_COMPLETION_INPUT"; then
    continue
  fi
  case $TEST_FILE in
  completion/*help*) IS_SERIAL_COMPLETION=yes ;;
  *) IS_SERIAL_COMPLETION=no ;;
  esac
  if word_is_listed "$TEST_FILE" "$SERIAL_COMPLETION_CANDIDATES" || \
    [ "$IS_SERIAL_COMPLETION" = yes ]; then
    SERIAL_COMPLETION_INPUT="$SERIAL_COMPLETION_INPUT $TEST_FILE"
  else
    PARALLEL_COMPLETION_INPUT="$PARALLEL_COMPLETION_INPUT $TEST_FILE"
  fi
done

HIGHLIGHT_INPUT=$(printf '%s ' highlight/*.sh)
INTERACTIVE_INPUT=$(printf '%s ' interactive/*.py)
SH_COMPAT_FILES=
for TEST_FILE in sh/*.sh; do
  case $TEST_FILE in
  *_1.sh) ;;
  *) SH_COMPAT_FILES="$SH_COMPAT_FILES $TEST_FILE" ;;
  esac
done
BASH_COMPAT_FILES=
for TEST_FILE in bash/*.bash; do
  case $TEST_FILE in
  *_1.bash) ;;
  *) BASH_COMPAT_FILES="$BASH_COMPAT_FILES $TEST_FILE" ;;
  esac
done

wait_for_workers()
{
  for WORKER_PID in $WORKER_PIDS; do
    wait "$WORKER_PID" || WORKER_STATUS=$?
  done
  WORKER_PIDS=
  WORKER_COUNT=0
}

wait_for_worker_slot()
{
  while [ "$WORKER_COUNT" -ge "$TEST_JOBS" ]; do
    REMAINING_WORKER_PIDS=
    DID_REAP_WORKER=no
    for WORKER_PID in $WORKER_PIDS; do
      if kill -0 "$WORKER_PID" 2>/dev/null; then
        REMAINING_WORKER_PIDS="$REMAINING_WORKER_PIDS $WORKER_PID"
        continue
      fi
      wait "$WORKER_PID" || WORKER_STATUS=$?
      WORKER_COUNT=$((WORKER_COUNT - 1))
      DID_REAP_WORKER=yes
    done
    WORKER_PIDS=$REMAINING_WORKER_PIDS
    if [ "$DID_REAP_WORKER" = no ]; then
      sleep 0.01
    fi
  done
}

run_harness_item()
{
  HARNESS_NAME=$1
  TEST_ITEM=$2

  case $HARNESS_NAME in
  kosh)
    "$TEST_SHELL" run-kosh-test.sh "$TEST_ITEM"
    ;;
  cli)
    "$TEST_SHELL" run-cli-test.sh "$TEST_SHELL" "$TEST_ITEM"
    ;;
  build)
    "$TEST_SHELL" run-build-test.sh "$TEST_SHELL" "$TEST_ITEM"
    ;;
  completion)
    "$TEST_SHELL" run-completion-test.sh "$TEST_SHELL" "$TEST_ITEM"
    ;;
  highlight)
    "$TEST_SHELL" run-highlight-test.sh "$TEST_SHELL" "$TEST_ITEM"
    ;;
  esac
}

run_parallel_harness()
{
  HARNESS_NAME=$1
  TEST_ITEMS=$2
  WORKER_STATUS=0

  for TEST_ITEM in $TEST_ITEMS; do
    run_harness_item "$HARNESS_NAME" "$TEST_ITEM" &
    WORKER_PIDS="$WORKER_PIDS $!"
    WORKER_COUNT=$((WORKER_COUNT + 1))
    wait_for_worker_slot
  done
  wait_for_workers

  return "$WORKER_STATUS"
}

print_platform_skips()
{
  HARNESS_NAME=$1

  case $HARNESS_NAME in
  kosh)
    for TEST_NAME in $SKIPPED_TEST_NAMES; do
      printf "\t%-64s skipped, unsupported Windows backend feature\n" \
        "$TEST_NAME.kosh"
    done
    ;;
  cli)
    if [ -n "${SKIP_CLI_ASSIMILATE-}" ]; then
      printf "\t%-64s skipped, remote transaction requires POSIX\n" \
        cli/assimilate.sh
    fi
    if [ -n "${SKIP_CLI_BASH_XTRACEFD-}" ]; then
      printf "\t%-64s skipped, numbered descriptors are unsupported on Windows\n" \
        cli/bash_xtracefd.sh
    fi
    for TEST_FILE in $SKIPPED_CLI_INPUT; do
      printf "\t%-64s skipped, unsupported Windows backend feature\n" \
        "$TEST_FILE"
    done
    ;;
  build)
    for TEST_FILE in $SKIPPED_BUILD_INPUT; do
      printf "\t%-64s skipped, unsupported Windows build probe\n" \
        "$TEST_FILE"
    done
    ;;
  completion)
    for TEST_FILE in $SKIPPED_COMPLETION_INPUT; do
      printf "\t%-64s skipped, needs a POSIX helper executable\n" \
        "$TEST_FILE"
    done
    for TEST_FILE in $UNREPRESENTABLE_COMPLETION_INPUT; do
      printf "\t%-64s skipped, golden test filenames are unsupported on Windows\n" \
        "$TEST_FILE"
    done
    ;;
  esac
}

run_serial_cli_tests()
{
  for TEST_FILE in $SERIAL_CLI_INPUT; do
    printf "\t%-64s running\n" "cli_$(basename "$TEST_FILE" .sh)"
    run_harness_item cli "$TEST_FILE" || return $?
  done
}

run_serial_completion_tests()
{
  for TEST_FILE in $SERIAL_COMPLETION_INPUT; do
    printf "\t%-64s running\n" \
      "completion_$(basename "$TEST_FILE" .sh)"
    run_harness_item completion "$TEST_FILE" || return $?
  done
}

run_named_suite()
{
  HARNESS_NAME=$1

  print_platform_skips "$HARNESS_NAME"
  case $HARNESS_NAME in
  kosh)
    run_parallel_harness kosh "$ACTIVE_TEST_NAMES"
    ;;
  cli)
    run_parallel_harness cli "$PARALLEL_CLI_INPUT" && run_serial_cli_tests
    ;;
  build)
    run_parallel_harness build "$ACTIVE_BUILD_INPUT"
    ;;
  completion)
    run_parallel_harness completion "$PARALLEL_COMPLETION_INPUT" && \
      run_serial_completion_tests
    ;;
  highlight)
    run_parallel_harness highlight "$HIGHLIGHT_INPUT"
    ;;
  interactive)
    BIN="$BIN" "$TEST_SHELL" run-interactive-test.sh $INTERACTIVE_INPUT
    ;;
  compat)
    BIN="$BIN" BASHP="$BASHP" DASH="$DASH" \
      DIFF_FLAGS="$DIFF_FLAGS" FAILED_LIST="$FAILED_LIST" \
      SH_COMPAT_FILES="$SH_COMPAT_FILES" \
      BASH_COMPAT_FILES="$BASH_COMPAT_FILES" \
      "$TEST_SHELL" run-compat-diff-test.sh
    ;;
  esac
}

finish_results()
{
  touch "$FAILED_LIST"
  cat "$FAILED_LIST"
  if [ -s "$FAILED_LIST" ]; then
    return 1
  fi

  return 0
}

if [ "$SUITE_NAME" != all ]; then
  rm -f "$FAILED_LIST"
  run_named_suite "$SUITE_NAME"
  SUITE_STATUS=$?
  finish_results || SUITE_STATUS=$?
  exit "$SUITE_STATUS"
fi

START_TIME=$(date +%s)
SUITE_STATUS=0
rm -f "$FAILED_LIST" "$KOSH_HISTORY" "$KOSH_DIRECTORY_HISTORY"
test -n "$PWD/.test-work" && rm -rf "$PWD/.test-work"

if [ "$SUITE_STATUS" -eq 0 ]; then
  run_named_suite cli || SUITE_STATUS=$?
fi

COMPAT_FAILED_LIST="$FAILED_LIST.compat"
PARALLEL_FAILED_LIST="$FAILED_LIST.parallel"
if [ "$SUITE_STATUS" -eq 0 ] && [ "${TARGET-}" != Windows_NT ]; then
  rm -f "$COMPAT_FAILED_LIST" "$PARALLEL_FAILED_LIST"
  (
    FAILED_LIST=$PARALLEL_FAILED_LIST
    PARALLEL_STATUS=0
    for HARNESS_NAME in kosh build highlight; do
      if [ "$PARALLEL_STATUS" -eq 0 ]; then
        run_named_suite "$HARNESS_NAME" || PARALLEL_STATUS=$?
      fi
    done
    if [ "$PARALLEL_STATUS" -eq 0 ]; then
      run_named_suite completion || PARALLEL_STATUS=$?
    fi
    exit "$PARALLEL_STATUS"
  ) &
  PARALLEL_PROCESS=$!

  FAILED_LIST="$COMPAT_FAILED_LIST" run_named_suite compat || SUITE_STATUS=$?
  wait "$PARALLEL_PROCESS" || SUITE_STATUS=$?
  for PARTIAL_FAILED_LIST in "$COMPAT_FAILED_LIST" "$PARALLEL_FAILED_LIST"; do
    if [ -f "$PARTIAL_FAILED_LIST" ]; then
      cat "$PARTIAL_FAILED_LIST" >> "$FAILED_LIST"
      rm -f "$PARTIAL_FAILED_LIST"
    fi
  done

  if [ "$SUITE_STATUS" -eq 0 ]; then
    run_named_suite interactive || SUITE_STATUS=$?
  fi
else
  for HARNESS_NAME in kosh build highlight completion; do
    if [ "$SUITE_STATUS" -eq 0 ]; then
      run_named_suite "$HARNESS_NAME" || SUITE_STATUS=$?
    fi
  done
fi
finish_results || SUITE_STATUS=$?

ELAPSED_SECONDS=$(($(date +%s) - START_TIME))
printf "\nDebug test step completed in %s seconds\n" "$ELAPSED_SECONDS"
if [ "$ELAPSED_SECONDS" -gt 180 ]; then
  printf "Debug test step exceeded the 180 second soft limit\n"
fi
if [ "$ELAPSED_SECONDS" -gt 300 ]; then
  printf "Debug test step exceeded the 300 second hard limit\n"
  SUITE_STATUS=1
fi

exit "$SUITE_STATUS"
