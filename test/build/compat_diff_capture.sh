#!/bin/bash

#
#    This file is a part of the Koshka shell, (c) toiletbril, 2026
#    See the top-level LICENSE file for the licensing information.
#
# This file verifies that the compatibility runner compares standard error and
# exit status in addition to standard output. A separate fixture supplies fake
# shells with controlled streams and statuses without depending on host shell
# diagnostics.
#

d=$(mktemp -d)
trap 'test -n "$d" && "$TEST_SYSTEM_RM" -rf "$d"' EXIT

reference_shell=$d/reference-shell
candidate_shell=$d/candidate-shell
test_file=$d/case.sh
failed_list=$d/failed.diff

printf '%s\n' \
  '#!/bin/sh' \
  'if [ "$1" = -c ]; then printf "5.3"; exit 0; fi' \
  'printf "same stdout\n"' \
  'printf "reference stderr\n" >&2' \
  'exit 3' \
  > "$reference_shell"
printf '%s\n' \
  '#!/bin/sh' \
  'printf "same stdout\n"' \
  'exit 7' \
  > "$candidate_shell"
chmod +x "$reference_shell" "$candidate_shell"
: > "$test_file"

BIN=$candidate_shell \
BASHP=$reference_shell \
DASH=$reference_shell \
DIFF_FLAGS=-u \
FAILED_LIST=$failed_list \
SH_COMPAT_FILES=$test_file \
BASH_COMPAT_FILES= \
TEST_TEMP_DIRECTORY=$d \
TEST_SYSTEM_RM=$TEST_SYSTEM_RM \
  "$TEST_SHELL" run-compat-diff-test.sh >/dev/null

failure=$(< "$failed_list")
case $failure in
  *'+reference stderr'*)
    echo compatibility-runner-captures-stderr
    ;;
  *)
    echo compatibility-runner-missed-stderr
    ;;
esac
case $failure in
  *'-7'*'+3'*) echo compatibility-runner-captures-status ;;
  *) echo compatibility-runner-missed-status ;;
esac
