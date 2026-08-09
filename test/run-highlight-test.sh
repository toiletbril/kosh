#!/bin/bash
refill_mode=no
test_status=0
if [ "${1-}" = --refill ]; then
  refill_mode=yes
  shift
fi

test_shell=$1
shift

if [ "${IS_NONDEBUG_BUILD:-0}" = 1 ]; then
  printf "\t%-64s skipped, release binary\n" highlight
  exit 0
fi

for f in "$@"; do
  name=$(basename "$f" .sh)
  if [ "$refill_mode" = yes ]; then
    out="expected/.$name.out.tmp"
  else
    output_directory="$TEST_TEMP_DIRECTORY/results/highlight"
    mkdir -p "$output_directory"
    out="$output_directory/$name.out"
  fi

  BIN="$BIN" "$test_shell" "$f" > "$out" 2>/dev/null
  if [ "$refill_mode" = yes ]; then
    mv "$out" "expected/$name.out"
    printf "\t%-64s %s.out\n" "highlight/$name.sh" "$name"
    continue
  fi

  if diff $DIFF_FLAGS "expected/$name.out" "$out" >/dev/null 2>&1; then
    printf "\t%-64s ok\033[K\r" "highlight/$name.sh"
  else
    diff $DIFF_FLAGS "expected/$name.out" "$out" | \
      tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "highlight/$name.sh"
    test_status=1
  fi
  rm -f "$out"
done

exit "$test_status"
