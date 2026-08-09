#!/bin/bash

suite_name=${1:-all}
worker_pids=
worker_count=0
worker_status=0

word_is_listed()
{
  word=$1
  words=$2

  case " $words " in
  *" $word "*) return 0 ;;
  *) return 1 ;;
  esac
}

ACTIVE_TEST_NAMES=
for test_file in shit/*.shit; do
  test_name=${test_file#shit/}
  test_name=${test_name%.shit}
  if word_is_listed "$test_name" "$SKIPPED_TEST_NAMES"; then
    continue
  fi
  ACTIVE_TEST_NAMES="$ACTIVE_TEST_NAMES $test_name"
done

SERIAL_CLI_CANDIDATES="cli/set_option_state.sh cli/read_behavior.sh"
PARALLEL_CLI_INPUT=
SERIAL_CLI_INPUT=
for test_file in cli/*.sh; do
  if word_is_listed "$test_file" "$SKIPPED_CLI_INPUT"; then
    continue
  fi
  if [ -n "${SKIP_CLI_ASSIMILATE-}" ] && \
    [ "$test_file" = cli/assimilate.sh ]; then
    continue
  fi
  if [ -n "${SKIP_CLI_BASH_XTRACEFD-}" ] && \
    [ "$test_file" = cli/bash_xtracefd.sh ]; then
    continue
  fi
  if word_is_listed "$test_file" "$SERIAL_CLI_CANDIDATES"; then
    SERIAL_CLI_INPUT="$SERIAL_CLI_INPUT $test_file"
  else
    PARALLEL_CLI_INPUT="$PARALLEL_CLI_INPUT $test_file"
  fi
done

ACTIVE_BUILD_INPUT=
for test_file in build/*.sh; do
  if word_is_listed "$test_file" "$SKIPPED_BUILD_INPUT"; then
    continue
  fi
  ACTIVE_BUILD_INPUT="$ACTIVE_BUILD_INPUT $test_file"
done

SERIAL_COMPLETION_CANDIDATES=completion/editor_append_hot_path.sh
PARALLEL_COMPLETION_INPUT=
SERIAL_COMPLETION_INPUT=
for test_file in completion/*.sh; do
  if word_is_listed "$test_file" "$SKIPPED_COMPLETION_INPUT" || \
    word_is_listed "$test_file" "$UNREPRESENTABLE_COMPLETION_INPUT"; then
    continue
  fi
  case $test_file in
  completion/*help*) is_serial_completion=yes ;;
  *) is_serial_completion=no ;;
  esac
  if word_is_listed "$test_file" "$SERIAL_COMPLETION_CANDIDATES" || \
    [ "$is_serial_completion" = yes ]; then
    SERIAL_COMPLETION_INPUT="$SERIAL_COMPLETION_INPUT $test_file"
  else
    PARALLEL_COMPLETION_INPUT="$PARALLEL_COMPLETION_INPUT $test_file"
  fi
done

HIGHLIGHT_INPUT=$(printf '%s ' highlight/*.sh)
INTERACTIVE_INPUT=$(printf '%s ' interactive/*.py)
SH_COMPAT_FILES=
for test_file in sh/*.sh; do
  case $test_file in
  *_1.sh) ;;
  *) SH_COMPAT_FILES="$SH_COMPAT_FILES $test_file" ;;
  esac
done
BASH_COMPAT_FILES=
for test_file in bash/*.bash; do
  case $test_file in
  *_1.bash) ;;
  *) BASH_COMPAT_FILES="$BASH_COMPAT_FILES $test_file" ;;
  esac
done

wait_for_workers()
{
  for worker_pid in $worker_pids; do
    wait "$worker_pid" || worker_status=$?
  done
  worker_pids=
  worker_count=0
}

wait_for_worker_slot()
{
  while [ "$worker_count" -ge "$TEST_JOBS" ]; do
    remaining_worker_pids=
    did_reap_worker=no
    for worker_pid in $worker_pids; do
      if kill -0 "$worker_pid" 2>/dev/null; then
        remaining_worker_pids="$remaining_worker_pids $worker_pid"
        continue
      fi
      wait "$worker_pid" || worker_status=$?
      worker_count=$((worker_count - 1))
      did_reap_worker=yes
    done
    worker_pids=$remaining_worker_pids
    if [ "$did_reap_worker" = no ]; then
      sleep 0.01
    fi
  done
}

run_harness_item()
{
  harness_name=$1
  test_item=$2

  case $harness_name in
  shit)
    "$TEST_SHELL" run-shit-test.sh "$test_item"
    ;;
  cli)
    "$TEST_SHELL" run-cli-test.sh "$TEST_SHELL" "$test_item"
    ;;
  build)
    "$TEST_SHELL" run-build-test.sh "$TEST_SHELL" "$test_item"
    ;;
  completion)
    "$TEST_SHELL" run-completion-test.sh "$TEST_SHELL" "$test_item"
    ;;
  highlight)
    "$TEST_SHELL" run-highlight-test.sh "$TEST_SHELL" "$test_item"
    ;;
  esac
}

run_parallel_harness()
{
  harness_name=$1
  test_items=$2
  worker_status=0

  for test_item in $test_items; do
    run_harness_item "$harness_name" "$test_item" &
    worker_pids="$worker_pids $!"
    worker_count=$((worker_count + 1))
    wait_for_worker_slot
  done
  wait_for_workers

  return "$worker_status"
}

print_platform_skips()
{
  harness_name=$1

  case $harness_name in
  shit)
    for test_name in $SKIPPED_TEST_NAMES; do
      printf "\t%-64s skipped, unsupported Windows backend feature\n" \
        "$test_name.shit"
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
    for test_file in $SKIPPED_CLI_INPUT; do
      printf "\t%-64s skipped, unsupported Windows backend feature\n" \
        "$test_file"
    done
    ;;
  build)
    for test_file in $SKIPPED_BUILD_INPUT; do
      printf "\t%-64s skipped, unsupported Windows build probe\n" \
        "$test_file"
    done
    ;;
  completion)
    for test_file in $SKIPPED_COMPLETION_INPUT; do
      printf "\t%-64s skipped, needs a POSIX helper executable\n" \
        "$test_file"
    done
    for test_file in $UNREPRESENTABLE_COMPLETION_INPUT; do
      printf "\t%-64s skipped, golden test filenames are unsupported on Windows\n" \
        "$test_file"
    done
    ;;
  esac
}

run_serial_cli_tests()
{
  for test_file in $SERIAL_CLI_INPUT; do
    printf "\t%-64s running\n" "cli_$(basename "$test_file" .sh)"
    run_harness_item cli "$test_file" || return $?
  done
}

run_serial_completion_tests()
{
  for test_file in $SERIAL_COMPLETION_INPUT; do
    printf "\t%-64s running\n" \
      "completion_$(basename "$test_file" .sh)"
    run_harness_item completion "$test_file" || return $?
  done
}

run_named_suite()
{
  harness_name=$1

  print_platform_skips "$harness_name"
  case $harness_name in
  shit)
    run_parallel_harness shit "$ACTIVE_TEST_NAMES"
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

if [ "$suite_name" != all ]; then
  rm -f "$FAILED_LIST"
  run_named_suite "$suite_name"
  suite_status=$?
  finish_results || suite_status=$?
  exit "$suite_status"
fi

start_time=$(date +%s)
suite_status=0
rm -f "$FAILED_LIST" "$SHIT_HISTORY" "$SHIT_DIRECTORY_HISTORY"
test -n "$PWD/.test-work" && rm -rf "$PWD/.test-work"

if [ "$suite_status" -eq 0 ]; then
  run_named_suite cli || suite_status=$?
fi

compat_failed_list="$FAILED_LIST.compat"
parallel_failed_list="$FAILED_LIST.parallel"
if [ "$suite_status" -eq 0 ] && [ "${TARGET-}" != Windows_NT ]; then
  rm -f "$compat_failed_list" "$parallel_failed_list"
  (
    FAILED_LIST=$parallel_failed_list
    parallel_status=0
    for harness_name in shit build highlight; do
      if [ "$parallel_status" -eq 0 ]; then
        run_named_suite "$harness_name" || parallel_status=$?
      fi
    done
    if [ "$parallel_status" -eq 0 ]; then
      run_named_suite completion || parallel_status=$?
    fi
    exit "$parallel_status"
  ) &
  parallel_process=$!

  FAILED_LIST="$compat_failed_list" run_named_suite compat || suite_status=$?
  wait "$parallel_process" || suite_status=$?
  for partial_failed_list in "$compat_failed_list" "$parallel_failed_list"; do
    if [ -f "$partial_failed_list" ]; then
      cat "$partial_failed_list" >> "$FAILED_LIST"
      rm -f "$partial_failed_list"
    fi
  done

  if [ "$suite_status" -eq 0 ]; then
    run_named_suite interactive || suite_status=$?
  fi
else
  for harness_name in shit build highlight completion; do
    if [ "$suite_status" -eq 0 ]; then
      run_named_suite "$harness_name" || suite_status=$?
    fi
  done
fi
finish_results || suite_status=$?

elapsed_seconds=$(($(date +%s) - start_time))
printf "\nDebug test step completed in %s seconds\n" "$elapsed_seconds"
if [ "$elapsed_seconds" -gt 180 ]; then
  printf "Debug test step exceeded the 180 second soft limit\n"
fi
if [ "$elapsed_seconds" -gt 300 ]; then
  printf "Debug test step exceeded the 300 second hard limit\n"
  suite_status=1
fi

exit "$suite_status"
