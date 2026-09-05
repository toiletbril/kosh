#!/bin/bash

CAPTURE_DIRECTORY="$TEST_TEMP_DIRECTORY/compat-diff-capture.$$"
mkdir -p "$CAPTURE_DIRECTORY"
trap 'test -n "$CAPTURE_DIRECTORY" && "$TEST_SYSTEM_RM" -rf "$CAPTURE_DIRECTORY"' EXIT

capture_command() {
  if "$@" > "$CAPTURE_DIRECTORY/stdout" 2> "$CAPTURE_DIRECTORY/stderr"; then
    CAPTURED_STATUS=0
  else
    CAPTURED_STATUS=$?
  fi

  printf X >> "$CAPTURE_DIRECTORY/stdout"
  CAPTURED_STDOUT=$(< "$CAPTURE_DIRECTORY/stdout")
  CAPTURED_STDOUT=${CAPTURED_STDOUT%X}
  printf X >> "$CAPTURE_DIRECTORY/stderr"
  CAPTURED_STDERR=$(< "$CAPTURE_DIRECTORY/stderr")
  CAPTURED_STDERR=${CAPTURED_STDERR%X}
}

have_same_stderr_presence() {
  if [ -z "$1" ]; then
    [ -z "$2" ]
  else
    [ -n "$2" ]
  fi
}

append_result_diff() {
  local ACTUAL_LABEL=$1
  local REFERENCE_LABEL=$2
  local ACTUAL_STDOUT=$3
  local ACTUAL_STDERR=$4
  local ACTUAL_STATUS=$5
  local REFERENCE_STDOUT=$6
  local REFERENCE_STDERR=$7
  local REFERENCE_STATUS=$8

  diff $DIFF_FLAGS --label "$ACTUAL_LABEL" --label "$REFERENCE_LABEL" \
    <(printf 'stdout\n%s\nstderr\n%s\nstatus\n%s\n' \
      "$ACTUAL_STDOUT" "$ACTUAL_STDERR" "$ACTUAL_STATUS") \
    <(printf 'stdout\n%s\nstderr\n%s\nstatus\n%s\n' \
      "$REFERENCE_STDOUT" "$REFERENCE_STDERR" "$REFERENCE_STATUS") \
    >> "$FAILED_LIST"
}

compare_one() {
  local REFERENCE_SHELL=$1
  local TEST_FILE=$2
  local SUFFIX=$3
  local REFERENCE_LABEL=$4
  local MOOD=$5
  local EXPLICIT_STDOUT EXPLICIT_STDERR EXPLICIT_STATUS
  local MIMIC_STDOUT MIMIC_STDERR MIMIC_STATUS
  local REFERENCE_STDOUT REFERENCE_STDERR REFERENCE_STATUS
  local ALTERNATIVE_FILE ALTERNATIVE_STDOUT ALTERNATIVE_STDERR
  local ALTERNATIVE_STATUS
  local EXPLICIT_MATCHES=0 MIMIC_MATCHES=0

  capture_command "$BIN" --no-traces --mood "$MOOD" "$TEST_FILE"
  EXPLICIT_STDOUT=$CAPTURED_STDOUT
  EXPLICIT_STDERR=$CAPTURED_STDERR
  EXPLICIT_STATUS=$CAPTURED_STATUS
  capture_command "$BIN" --no-traces -I -c "$TEST_FILE"
  MIMIC_STDOUT=$CAPTURED_STDOUT
  MIMIC_STDERR=$CAPTURED_STDERR
  MIMIC_STATUS=$CAPTURED_STATUS
  capture_command "$REFERENCE_SHELL" "$TEST_FILE"
  REFERENCE_STDOUT=$CAPTURED_STDOUT
  REFERENCE_STDERR=$CAPTURED_STDERR
  REFERENCE_STATUS=$CAPTURED_STATUS

  if [ "$EXPLICIT_STDOUT" = "$REFERENCE_STDOUT" ] && \
    have_same_stderr_presence "$EXPLICIT_STDERR" "$REFERENCE_STDERR" && \
    [ "$EXPLICIT_STATUS" -eq "$REFERENCE_STATUS" ]; then
    EXPLICIT_MATCHES=1
  fi
  if [ "$MIMIC_STDOUT" = "$REFERENCE_STDOUT" ] && \
    have_same_stderr_presence "$MIMIC_STDERR" "$REFERENCE_STDERR" && \
    [ "$MIMIC_STATUS" -eq "$REFERENCE_STATUS" ]; then
    MIMIC_MATCHES=1
  fi

  ALTERNATIVE_FILE="${TEST_FILE%$SUFFIX}_1$SUFFIX"
  if [ -f "$ALTERNATIVE_FILE" ] && \
    { [ "$EXPLICIT_MATCHES" -eq 0 ] || [ "$MIMIC_MATCHES" -eq 0 ]; }
  then
    capture_command "$REFERENCE_SHELL" "$ALTERNATIVE_FILE"
    ALTERNATIVE_STDOUT=$CAPTURED_STDOUT
    ALTERNATIVE_STDERR=$CAPTURED_STDERR
    ALTERNATIVE_STATUS=$CAPTURED_STATUS
    if [ "$EXPLICIT_STDOUT" = "$ALTERNATIVE_STDOUT" ] && \
      have_same_stderr_presence "$EXPLICIT_STDERR" "$ALTERNATIVE_STDERR" && \
      [ "$EXPLICIT_STATUS" -eq "$ALTERNATIVE_STATUS" ]; then
      EXPLICIT_MATCHES=1
    fi
    if [ "$MIMIC_STDOUT" = "$ALTERNATIVE_STDOUT" ] && \
      have_same_stderr_presence "$MIMIC_STDERR" "$ALTERNATIVE_STDERR" && \
      [ "$MIMIC_STATUS" -eq "$ALTERNATIVE_STATUS" ]; then
      MIMIC_MATCHES=1
    fi
  fi

  if [ "$EXPLICIT_MATCHES" -eq 1 ] && [ "$MIMIC_MATCHES" -eq 1 ]; then
    printf "\t%-64s ok\033[K\r" "$TEST_FILE"
    return
  fi

  if [ "$EXPLICIT_MATCHES" -eq 0 ]; then
    append_result_diff "$TEST_FILE (kosh --mood $MOOD)" \
      "$TEST_FILE ($REFERENCE_LABEL)" \
      "$EXPLICIT_STDOUT" "$EXPLICIT_STDERR" "$EXPLICIT_STATUS" \
      "$REFERENCE_STDOUT" "$REFERENCE_STDERR" "$REFERENCE_STATUS"
  fi
  if [ "$MIMIC_MATCHES" -eq 0 ]; then
    append_result_diff "$TEST_FILE (kosh -I)" \
      "$TEST_FILE ($REFERENCE_LABEL)" \
      "$MIMIC_STDOUT" "$MIMIC_STDERR" "$MIMIC_STATUS" \
      "$REFERENCE_STDOUT" "$REFERENCE_STDERR" "$REFERENCE_STATUS"
  fi
  printf "\t%-64s FAILED :c\n" "$TEST_FILE"
}

BASH_SKIP_REASON=
if ! command -v "$BASHP" >/dev/null 2>&1; then
  BASH_SKIP_REASON="no $BASHP"
else
  BASH_VERSION_TEXT=$("$BASHP" -c \
    'printf "%s.%s" "${BASH_VERSINFO[0]}" "${BASH_VERSINFO[1]}"' 2>/dev/null)
  BASH_MAJOR_VERSION=${BASH_VERSION_TEXT%%.*}
  BASH_MINOR_VERSION=${BASH_VERSION_TEXT#*.}
  if [ -z "$BASH_MAJOR_VERSION" ] || [ "$BASH_MAJOR_VERSION" -lt 5 ] || \
    { [ "$BASH_MAJOR_VERSION" -eq 5 ] && [ "$BASH_MINOR_VERSION" -lt 3 ]; }
  then
    BASH_SKIP_REASON="$BASHP is bash ${BASH_VERSION_TEXT:-unknown}, need 5.3+"
  fi
fi

if [ -z "$BASH_SKIP_REASON" ]; then
  for TEST_FILE in $BASH_COMPAT_FILES; do
    compare_one "$BASHP" "$TEST_FILE" .bash bash bash
  done
else
  printf "\t%-64s skipped, %s\n" bashdiff "$BASH_SKIP_REASON"
fi

if command -v "$DASH" >/dev/null 2>&1; then
  for TEST_FILE in $SH_COMPAT_FILES; do
    compare_one "$DASH" "$TEST_FILE" .sh dash sh
  done
else
  printf "\t%-64s skipped, no %s\n" dashdiff "$DASH"
fi
