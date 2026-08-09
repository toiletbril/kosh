#!/bin/bash

TEST_SHELL_COMMAND=$1

if [ -n "${REFILL-}" ]; then
  "$TEST_SHELL_COMMAND" run-shit-test.sh --refill $REFILL
  for TEST_NAME in $REFILL; do
    if [ -f "cli/$TEST_NAME.sh" ]; then
      "$TEST_SHELL_COMMAND" run-cli-test.sh --refill \
        "$TEST_SHELL_COMMAND" "cli/$TEST_NAME.sh"
    fi
    if [ -f "completion/$TEST_NAME.sh" ]; then
      "$TEST_SHELL_COMMAND" run-completion-test.sh --refill \
        "$TEST_SHELL_COMMAND" "completion/$TEST_NAME.sh"
    fi
    if [ -f "highlight/$TEST_NAME.sh" ]; then
      "$TEST_SHELL_COMMAND" run-highlight-test.sh --refill \
        "$TEST_SHELL_COMMAND" "highlight/$TEST_NAME.sh"
    fi
  done
  exit 0
fi

NATIVE_TEST_NAMES=
for TEST_FILE in shit/*.shit; do
  TEST_NAME=${TEST_FILE#shit/}
  NATIVE_TEST_NAMES="$NATIVE_TEST_NAMES ${TEST_NAME%.shit}"
done
"$TEST_SHELL_COMMAND" run-shit-test.sh --refill $NATIVE_TEST_NAMES
"$TEST_SHELL_COMMAND" run-cli-test.sh --refill "$TEST_SHELL_COMMAND" cli/*.sh
"$TEST_SHELL_COMMAND" run-completion-test.sh --refill \
  "$TEST_SHELL_COMMAND" completion/*.sh
"$TEST_SHELL_COMMAND" run-highlight-test.sh --refill \
  "$TEST_SHELL_COMMAND" highlight/*.sh
