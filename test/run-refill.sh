#!/bin/bash

TEST_SHELL_COMMAND=$1
REFILL_STATUS=0

word_is_listed()
{
  WORD=$1
  WORDS=$2

  case " $WORDS " in
  *" $WORD "*) return 0 ;;
  *) return 1 ;;
  esac
}

# Refills one harness and keeps the first failing status. The named runner
# receives the refill flag and the remaining operands.
refill_harness()
{
  RUNNER_NAME=$1
  shift
  RUNNER_STATUS=0
  "$TEST_SHELL_COMMAND" "$RUNNER_NAME" --refill "$@" || RUNNER_STATUS=$?
  [ "$REFILL_STATUS" -ne 0 ] || REFILL_STATUS=$RUNNER_STATUS
}

if [ -n "${REFILL-}" ]; then
  refill_harness run-kosh-test.sh $REFILL

  for TEST_NAME in $REFILL; do
    DID_FIND_TEST=no
    if [ -f "kosh/$TEST_NAME.kosh" ]; then
      DID_FIND_TEST=yes
    fi

    if [ -f "cli/$TEST_NAME.sh" ]; then
      DID_FIND_TEST=yes
      if ! word_is_listed "cli/$TEST_NAME.sh" "$SKIPPED_CLI_INPUT"; then
        refill_harness run-cli-test.sh "$TEST_SHELL_COMMAND" \
          "cli/$TEST_NAME.sh"
      fi
    fi

    if [ -f "build/$TEST_NAME.sh" ]; then
      DID_FIND_TEST=yes
      refill_harness run-build-test.sh "$TEST_SHELL_COMMAND" \
        "build/$TEST_NAME.sh"
    fi

    if [ -f "completion/$TEST_NAME.sh" ]; then
      DID_FIND_TEST=yes
      refill_harness run-completion-test.sh "$TEST_SHELL_COMMAND" \
        "completion/$TEST_NAME.sh"
    fi

    if [ -f "highlight/$TEST_NAME.sh" ]; then
      DID_FIND_TEST=yes
      refill_harness run-highlight-test.sh "$TEST_SHELL_COMMAND" \
        "highlight/$TEST_NAME.sh"
    fi

    if [ "$DID_FIND_TEST" = no ]; then
      printf "Unknown refill test: %s\n" "$TEST_NAME" >&2
      [ "$REFILL_STATUS" -ne 0 ] || REFILL_STATUS=1
    fi
  done

  exit "$REFILL_STATUS"
fi

NATIVE_TEST_NAMES=
for TEST_FILE in kosh/*.kosh; do
  TEST_NAME=${TEST_FILE#kosh/}
  NATIVE_TEST_NAMES="$NATIVE_TEST_NAMES ${TEST_NAME%.kosh}"
done

refill_harness run-kosh-test.sh $NATIVE_TEST_NAMES
ACTIVE_CLI_INPUT=
for TEST_FILE in cli/*.sh; do
  if word_is_listed "$TEST_FILE" "$SKIPPED_CLI_INPUT"; then
    continue
  fi
  ACTIVE_CLI_INPUT="$ACTIVE_CLI_INPUT $TEST_FILE"
done
refill_harness run-cli-test.sh "$TEST_SHELL_COMMAND" $ACTIVE_CLI_INPUT
refill_harness run-build-test.sh "$TEST_SHELL_COMMAND" build/*.sh
refill_harness run-completion-test.sh "$TEST_SHELL_COMMAND" completion/*.sh
refill_harness run-highlight-test.sh "$TEST_SHELL_COMMAND" highlight/*.sh

exit "$REFILL_STATUS"
