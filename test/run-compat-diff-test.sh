#!/bin/bash

compare_one() {
  local reference_shell=$1
  local file=$2
  local suffix=$3
  local label=$4
  local mood=$5
  local explicit_output mimic_output reference_output alternative_file alternative_output
  local explicit_matches=0 mimic_matches=0

  explicit_output="$("$BIN" --mood "$mood" "$file" 2>/dev/null; printf X)"
  explicit_output=${explicit_output%X}
  mimic_output="$("$BIN" -I -c "$file" 2>/dev/null; printf X)"
  mimic_output=${mimic_output%X}
  reference_output="$("$reference_shell" "$file" 2>/dev/null; printf X)"
  reference_output=${reference_output%X}

  if [ "$explicit_output" = "$reference_output" ]; then
    explicit_matches=1
  fi
  if [ "$mimic_output" = "$reference_output" ]; then
    mimic_matches=1
  fi

  alternative_file="${file%$suffix}_1$suffix"
  if [ -f "$alternative_file" ] && \
    { [ "$explicit_matches" -eq 0 ] || [ "$mimic_matches" -eq 0 ]; }
  then
    alternative_output="$("$reference_shell" "$alternative_file" 2>/dev/null; printf X)"
    alternative_output=${alternative_output%X}
    if [ "$explicit_output" = "$alternative_output" ]; then
      explicit_matches=1
    fi
    if [ "$mimic_output" = "$alternative_output" ]; then
      mimic_matches=1
    fi
  fi

  if [ "$explicit_matches" -eq 1 ] && [ "$mimic_matches" -eq 1 ]; then
    printf "\t%-64s ok\033[K\r" "$file"
    return
  fi

  if [ "$explicit_matches" -eq 0 ]; then
    diff $DIFF_FLAGS --label "$file (shit --mood $mood)" \
      --label "$file ($label)" \
      <(printf '%s' "$explicit_output") \
      <(printf '%s' "$reference_output") >> "$FAILED_LIST"
  fi
  if [ "$mimic_matches" -eq 0 ]; then
    diff $DIFF_FLAGS --label "$file (shit -I)" \
      --label "$file ($label)" \
      <(printf '%s' "$mimic_output") \
      <(printf '%s' "$reference_output") >> "$FAILED_LIST"
  fi
  printf "\t%-64s FAILED :c\n" "$file"
}

bash_skip_reason=
if ! command -v "$BASHP" >/dev/null 2>&1; then
  bash_skip_reason="no $BASHP"
else
  bash_version=$("$BASHP" -c 'printf "%s.%s" "${BASH_VERSINFO[0]}" "${BASH_VERSINFO[1]}"' 2>/dev/null)
  bash_major=${bash_version%%.*}
  bash_minor=${bash_version#*.}
  if [ -z "$bash_major" ] || [ "$bash_major" -lt 5 ] || \
    { [ "$bash_major" -eq 5 ] && [ "$bash_minor" -lt 3 ]; }
  then
    bash_skip_reason="$BASHP is bash ${bash_version:-unknown}, need 5.3+"
  fi
fi

if [ -z "$bash_skip_reason" ]; then
  for file in $BASH_COMPAT_FILES; do
    compare_one "$BASHP" "$file" .bash bash bash
  done
else
  printf "\t%-64s skipped, %s\n" bashdiff "$bash_skip_reason"
fi

if command -v "$DASH" >/dev/null 2>&1; then
  for file in $SH_COMPAT_FILES; do
    compare_one "$DASH" "$file" .sh dash sh
  done
else
  printf "\t%-64s skipped, no %s\n" dashdiff "$DASH"
fi
