#!/bin/sh

left="$TEST_TEMP_DIRECTORY/diff-bootstrap-left"
right="$TEST_TEMP_DIRECTORY/diff-bootstrap-right"
mkdir -p "$TEST_TEMP_DIRECTORY" || exit 1
trap 'test -n "$left" && test -n "$right" && "$TEST_SYSTEM_RM" -f "$left" "$right"' EXIT

printf 'one\ntwo\nthree\n' > "$left"
printf 'one\nTWO\nthree\n' > "$right"
output=$(koshkit diff -u -w -a -L left -L right "$left" "$right")
status=$?
expected=$(printf '%s\n' \
  '--- left' \
  '+++ right' \
  '@@ -1,3 +1,3 @@' \
  ' one' \
  '-two' \
  '+TWO' \
  ' three')

test "$status" -eq 1 && test "$output" = "$expected" || exit 1

printf 'one  two\r\n' > "$left"
printf 'one two\n' > "$right"
output=$(koshkit diff -u -w -a "$left" "$right")
status=$?

test "$status" -eq 0 && test -z "$output"
