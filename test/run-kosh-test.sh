#!/bin/bash
REFILL_MODE=no
TEST_STATUS=0
if [ "${1-}" = --refill ]; then
  REFILL_MODE=yes
  shift
fi

for TEST_NAME in "$@"; do
  [ -f "kosh/$TEST_NAME.kosh" ] || continue
  case $TEST_NAME in
  shellcheck_static_*) TEST_BIN_FLAGS="$BIN_FLAGS -WWW" ;;
  *) TEST_BIN_FLAGS="$BIN_FLAGS -WWW --no-annoying-diagnostics" ;;
  esac
  if [ "$REFILL_MODE" = yes ]; then
    OUTPUT="expected/.$TEST_NAME.out.tmp"
    "$BIN" $TEST_BIN_FLAGS - < "kosh/$TEST_NAME.kosh" > "$OUTPUT" 2>&1
    mv "$OUTPUT" "expected/$TEST_NAME.out"
    printf "\t%-64s %s.out\n" "$TEST_NAME.kosh" "$TEST_NAME"
    continue
  fi

  OUTPUT_DIRECTORY="$TEST_TEMP_DIRECTORY/results/kosh"
  mkdir -p "$OUTPUT_DIRECTORY"
  OUTPUT="$OUTPUT_DIRECTORY/$TEST_NAME.out"
  "$BIN" $TEST_BIN_FLAGS - < "kosh/$TEST_NAME.kosh" > "$OUTPUT" 2>&1
  if diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" >/dev/null 2>&1 || \
    { [ -f "expected/${TEST_NAME}_1.out" ] && \
      diff $DIFF_FLAGS "expected/${TEST_NAME}_1.out" "$OUTPUT" >/dev/null 2>&1; }; then
    printf "\t%-64s ok\033[K\r" "$TEST_NAME.kosh"
  else
    diff $DIFF_FLAGS "expected/$TEST_NAME.out" "$OUTPUT" | tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "$TEST_NAME.kosh"
    TEST_STATUS=1
  fi
  rm -f "$OUTPUT"
done

exit "$TEST_STATUS"
