#!/bin/bash
refill_mode=no
test_status=0
if [ "${1-}" = --refill ]; then
  refill_mode=yes
  shift
fi

test_shell=$1
shift

for f in "$@"; do
  name=$(basename "$f" .sh)
  case $name in
  command_substitution_strategy|fg_terminal_handoff)
    if [ "${IS_NONDEBUG_BUILD:-0}" = 1 ]; then
      printf "\t%-64s skipped, release binary\n" "cli/$name.sh"
      continue
    fi
    ;;
  esac
  if [ "$refill_mode" = yes ]; then
    out="expected/.$name.out.tmp"
  else
    output_directory="$TEST_TEMP_DIRECTORY/results/cli"
    mkdir -p "$output_directory"
    out="$output_directory/$name.out"
  fi

  case $name in
  command_substitution_interrupt|fg_terminal_handoff|history_behavior|read_behavior|\
    shitbox_timeout|transaction_lock_lifetime|wait_on_stopped_job)
    golden_timeout_seconds=60
    if [ "$name" = history_behavior ] || [ "$name" = shitbox_timeout ]; then
      golden_timeout_seconds=120
    fi
    CLI_TEST_TIMEOUT_SECONDS=${CLI_TEST_TIMEOUT_SECONDS:-$golden_timeout_seconds} \
      BIN="$BIN" "$test_shell" ./run-bounded-cli-golden.sh "$f" \
      > "$out" 2>&1
    driver_status=$?
    if [ "$refill_mode" = yes ] && [ "$driver_status" -ne 0 ]; then
      rm -f "$out"
      exit "$driver_status"
    fi
    if [ "$refill_mode" = no ] && [ "$driver_status" -ne 0 ]; then
      printf 'golden exited with status %s\n' "$driver_status" >> "$out"
    fi
    ;;
  *)
    BIN="$BIN" "$test_shell" "$f" > "$out" 2>&1
    ;;
  esac
  if [ "$refill_mode" = yes ]; then
    mv "$out" "expected/$name.out"
    printf "\t%-64s cli/%s.out\n" "cli/$name.sh" "$name"
    continue
  fi

  if diff $DIFF_FLAGS "expected/$name.out" "$out" >/dev/null 2>&1; then
    printf "\t%-64s ok\033[K\r" "cli/$name.sh"
  else
    diff $DIFF_FLAGS "expected/$name.out" "$out" | tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "cli/$name.sh"
    test_status=1
  fi
  rm -f "$out"
done

exit "$test_status"
