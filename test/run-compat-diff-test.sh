#!/bin/bash

compare_one() {
  local REFERENCE_SHELL=$1
  local TEST_FILE=$2
  local SUFFIX=$3
  local REFERENCE_LABEL=$4
  local MOOD=$5
  local EXPLICIT_OUTPUT MIMIC_OUTPUT REFERENCE_OUTPUT ALTERNATIVE_FILE
  local ALTERNATIVE_OUTPUT
  local EXPLICIT_MATCHES=0 MIMIC_MATCHES=0

  EXPLICIT_OUTPUT="$("$BIN" --mood "$MOOD" "$TEST_FILE" 2>/dev/null; printf X)"
  EXPLICIT_OUTPUT=${EXPLICIT_OUTPUT%X}
  MIMIC_OUTPUT="$("$BIN" -I -c "$TEST_FILE" 2>/dev/null; printf X)"
  MIMIC_OUTPUT=${MIMIC_OUTPUT%X}
  REFERENCE_OUTPUT="$("$REFERENCE_SHELL" "$TEST_FILE" 2>/dev/null; printf X)"
  REFERENCE_OUTPUT=${REFERENCE_OUTPUT%X}

  if [ "$EXPLICIT_OUTPUT" = "$REFERENCE_OUTPUT" ]; then
    EXPLICIT_MATCHES=1
  fi
  if [ "$MIMIC_OUTPUT" = "$REFERENCE_OUTPUT" ]; then
    MIMIC_MATCHES=1
  fi

  ALTERNATIVE_FILE="${TEST_FILE%$SUFFIX}_1$SUFFIX"
  if [ -f "$ALTERNATIVE_FILE" ] && \
    { [ "$EXPLICIT_MATCHES" -eq 0 ] || [ "$MIMIC_MATCHES" -eq 0 ]; }
  then
    ALTERNATIVE_OUTPUT="$("$REFERENCE_SHELL" "$ALTERNATIVE_FILE" 2>/dev/null; printf X)"
    ALTERNATIVE_OUTPUT=${ALTERNATIVE_OUTPUT%X}
    if [ "$EXPLICIT_OUTPUT" = "$ALTERNATIVE_OUTPUT" ]; then
      EXPLICIT_MATCHES=1
    fi
    if [ "$MIMIC_OUTPUT" = "$ALTERNATIVE_OUTPUT" ]; then
      MIMIC_MATCHES=1
    fi
  fi

  if [ "$EXPLICIT_MATCHES" -eq 1 ] && [ "$MIMIC_MATCHES" -eq 1 ]; then
    printf "\t%-64s ok\033[K\r" "$TEST_FILE"
    return
  fi

  if [ "$EXPLICIT_MATCHES" -eq 0 ]; then
    diff $DIFF_FLAGS --label "$TEST_FILE (shit --mood $MOOD)" \
      --label "$TEST_FILE ($REFERENCE_LABEL)" \
      <(printf '%s' "$EXPLICIT_OUTPUT") \
      <(printf '%s' "$REFERENCE_OUTPUT") >> "$FAILED_LIST"
  fi
  if [ "$MIMIC_MATCHES" -eq 0 ]; then
    diff $DIFF_FLAGS --label "$TEST_FILE (shit -I)" \
      --label "$TEST_FILE ($REFERENCE_LABEL)" \
      <(printf '%s' "$MIMIC_OUTPUT") \
      <(printf '%s' "$REFERENCE_OUTPUT") >> "$FAILED_LIST"
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
