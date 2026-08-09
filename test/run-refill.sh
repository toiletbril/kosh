#!/bin/bash

test_shell=$1

if [ -n "${REFILL-}" ]; then
  "$test_shell" run-shit-test.sh --refill $REFILL
  for name in $REFILL; do
    [ -f "cli/$name.sh" ] || continue
    "$test_shell" run-cli-test.sh --refill "$test_shell" "cli/$name.sh"
  done
  exit 0
fi

native_names=
for test_file in shit/*.shit; do
  name=${test_file#shit/}
  native_names="$native_names ${name%.shit}"
done
"$test_shell" run-shit-test.sh --refill $native_names
"$test_shell" run-cli-test.sh --refill "$test_shell" cli/*.sh
"$test_shell" run-completion-test.sh --refill "$test_shell" completion/*.sh
"$test_shell" run-highlight-test.sh --refill "$test_shell" highlight/*.sh
