#!/bin/bash
refill_mode=no
test_status=0
if [ "${1-}" = --refill ]; then
  refill_mode=yes
  shift
fi

for name in "$@"; do
  [ -f "shit/$name.shit" ] || continue
  case $name in
  shellcheck_static) test_bin_flags="$BIN_FLAGS -W" ;;
  *) test_bin_flags="$BIN_FLAGS --no-annoying-diagnostics" ;;
  esac
  if [ "$refill_mode" = yes ]; then
    out="expected/.$name.out.tmp"
    "$BIN" $test_bin_flags - < "shit/$name.shit" > "$out" 2>&1
    mv "$out" "expected/$name.out"
    printf "\t%-64s %s.out\n" "$name.shit" "$name"
    continue
  fi

  output_directory="$TEST_TEMP_DIRECTORY/results/shit"
  mkdir -p "$output_directory"
  out="$output_directory/$name.out"
  "$BIN" $test_bin_flags - < "shit/$name.shit" > "$out" 2>&1
  if diff $DIFF_FLAGS "expected/$name.out" "$out" >/dev/null 2>&1 || \
    { [ -f "expected/${name}_1.out" ] && \
      diff $DIFF_FLAGS "expected/${name}_1.out" "$out" >/dev/null 2>&1; }; then
    printf "\t%-64s ok\033[K\r" "$name.shit"
  else
    diff $DIFF_FLAGS "expected/$name.out" "$out" | tee -a "$FAILED_LIST"
    printf "\t%-64s FAILED :c\n" "$name.shit"
    test_status=1
  fi
  rm -f "$out"
done

exit "$test_status"
