#!/bin/bash
# Benchmark configure.sh, configure.bash, and configure.shit across the reference
# shells and shit, reporting wall-clock seconds at the given scale and checking
# that shit output matches the reference shell. The Makefile passes SCALE, BIN,
# DASH, BASHP, ZSH, ASH, YASH, PYTHON, BENCH, BENCH_BASH, and BENCH_SHIT. Run
# from the test directory. The bash time keyword formats the wall clock through
# TIMEFORMAT.

set -euo pipefail

export TIMEFORMAT="%R"

WORK=$TEST_TEMP_DIRECTORY/bench
cleanup_benchmark_work() {
  if [ -n "${WORK-}" ]; then /bin/rm -rf "$WORK"; fi
}

cleanup_benchmark_work
mkdir -p "$WORK"
trap cleanup_benchmark_work EXIT
D=$WORK/d
B=$WORK/b
Z=$WORK/z
G=$WORK/g
L=$WORK/l
S=$WORK/s
BB=$WORK/bb
SB=$WORK/sb
ZB=$WORK/zb
ZS=$WORK/zs
PP=$WORK/pp
PB=$WORK/pb
PS=$WORK/ps
TIME_OUTPUT=$WORK/time
ERROR_OUTPUT=$WORK/error
DIFF_OUTPUT=$WORK/diff

require_executable() {
  local EXECUTABLE=$1

  if command -v "$EXECUTABLE" >/dev/null 2>&1; then return 0; fi

  echo "required benchmark executable '$EXECUTABLE' was not found" >&2
  return 1
}

measure_command() {
  local OUTPUT=$1
  local STATUS
  local TIMING
  shift

  if { time SCALE="$SCALE" "$@" >"$OUTPUT" 2>"$ERROR_OUTPUT"; } \
    2>"$TIME_OUTPUT"
  then
    STATUS=0
  else
    STATUS=$?
  fi
  if [ "$STATUS" -ne 0 ]; then
    echo "benchmark command failed with status $STATUS" >&2
    printf '  command' >&2
    printf ' %q' "$@" >&2
    printf '\n' >&2
    sed -n '1,20p' "$OUTPUT" >&2
    sed -n '1,20p' "$ERROR_OUTPUT" >&2
    return "$STATUS"
  fi

  TIMING=$(<"$TIME_OUTPUT")
  if [[ ! $TIMING =~ ^[0-9]+[.][0-9]{3}$ ]]; then
    echo "benchmark command produced an invalid timing '$TIMING'" >&2
    return 1
  fi
  LAST_TIMING_MILLISECONDS=$((10#${TIMING/./}))
  if ((LAST_TIMING_MILLISECONDS == 0)); then
    echo "benchmark command produced an invalid timing '$TIMING'" >&2
    return 1
  fi
  LAST_TIMING=$TIMING
}

run_timed() {
  local LABEL=$1
  local OUTPUT=$2
  shift 2

  measure_command "$OUTPUT" "$@"
  printf "  %-16s%s\n" "$LABEL" "$LAST_TIMING"
}

compare_outputs() {
  local EXPECTED=$1
  local ACTUAL=$2
  local REFERENCE_NAME=$3
  local REPORT_MATCH=${4:-yes}

  if cmp -s "$EXPECTED" "$ACTUAL"; then
    if [ "$REPORT_MATCH" = yes ]; then echo "output matches $REFERENCE_NAME"; fi
    return 0
  fi

  diff -u "$EXPECTED" "$ACTUAL" >"$DIFF_OUTPUT" || true
  echo "output differs from $REFERENCE_NAME:"
  sed -n '1,20p' "$DIFF_OUTPUT"
  return 1
}

run_mode_command() {
  local MODE=$1
  local IMPLEMENTATION=$2

  case "$MODE:$IMPLEMENTATION" in
    sh:reference)
      measure_command "$D" "$DASH" "$BENCH"
      ;;
    sh:candidate)
      measure_command "$S" "$BIN" --mood sh --no-diagnostics "$BENCH"
      ;;
    bash:reference)
      measure_command "$BB" "$BASHP" "$BENCH_BASH"
      ;;
    bash:candidate)
      measure_command "$SB" "$BIN" --mood bash --no-diagnostics "$BENCH_BASH"
      ;;
    *)
      echo "unknown benchmark mode '$MODE:$IMPLEMENTATION'" >&2
      return 1
      ;;
  esac
}

run_pair() {
  local MODE=$1
  local ORDER=$2
  local EXPECTED
  local ACTUAL
  local REFERENCE_NAME

  if [ "$ORDER" = reference-first ]; then
    run_mode_command "$MODE" reference
    PAIR_REFERENCE_TIMING=$LAST_TIMING
    PAIR_REFERENCE_MILLISECONDS=$LAST_TIMING_MILLISECONDS
    run_mode_command "$MODE" candidate
    PAIR_CANDIDATE_TIMING=$LAST_TIMING
    PAIR_CANDIDATE_MILLISECONDS=$LAST_TIMING_MILLISECONDS
  else
    run_mode_command "$MODE" candidate
    PAIR_CANDIDATE_TIMING=$LAST_TIMING
    PAIR_CANDIDATE_MILLISECONDS=$LAST_TIMING_MILLISECONDS
    run_mode_command "$MODE" reference
    PAIR_REFERENCE_TIMING=$LAST_TIMING
    PAIR_REFERENCE_MILLISECONDS=$LAST_TIMING_MILLISECONDS
  fi

  if [ "$MODE" = sh ]; then
    EXPECTED=$D
    ACTUAL=$S
    REFERENCE_NAME=dash
  else
    EXPECTED=$BB
    ACTUAL=$SB
    REFERENCE_NAME=bash
  fi
  compare_outputs "$EXPECTED" "$ACTUAL" "$REFERENCE_NAME" no
}

benchmark_pair() {
  local MODE=$1
  local REFERENCE_LABEL
  local CANDIDATE_LABEL
  local REFERENCE_NAME
  local REQUIRED_SPEEDUP
  local SAMPLE_INDEX
  local ORDER
  local REFERENCE_MAXIMUM
  local REFERENCE_MAXIMUM_MILLISECONDS
  local CANDIDATE_MINIMUM
  local CANDIDATE_MINIMUM_MILLISECONDS
  local SPEEDUP_THOUSANDTHS

  if [ "$MODE" = sh ]; then
    REFERENCE_LABEL=$(basename "$DASH")
    REFERENCE_NAME=dash
    REQUIRED_SPEEDUP=$DASH_SPEEDUP_REQUIRED
  else
    REFERENCE_LABEL=$(basename "$BASHP")
    REFERENCE_NAME=bash
    REQUIRED_SPEEDUP=$BASH_SPEEDUP_REQUIRED
  fi
  CANDIDATE_LABEL=$(basename "$BIN")

  for ((SAMPLE_INDEX = 1;
        SAMPLE_INDEX <= BENCH_WARMUP_COUNT;
        SAMPLE_INDEX++)); do
    if ((SAMPLE_INDEX % 2 == 1)); then
      ORDER=reference-first
    else
      ORDER=candidate-first
    fi
    run_pair "$MODE" "$ORDER"
  done

  for ((SAMPLE_INDEX = 1;
        SAMPLE_INDEX <= BENCH_SAMPLE_COUNT;
        SAMPLE_INDEX++)); do
    if ((SAMPLE_INDEX % 2 == 1)); then
      ORDER=reference-first
    else
      ORDER=candidate-first
    fi
    run_pair "$MODE" "$ORDER"

    if ((SAMPLE_INDEX == 1 ||
         PAIR_REFERENCE_MILLISECONDS > REFERENCE_MAXIMUM_MILLISECONDS)); then
      REFERENCE_MAXIMUM=$PAIR_REFERENCE_TIMING
      REFERENCE_MAXIMUM_MILLISECONDS=$PAIR_REFERENCE_MILLISECONDS
    fi
    if ((SAMPLE_INDEX == 1 ||
         PAIR_CANDIDATE_MILLISECONDS < CANDIDATE_MINIMUM_MILLISECONDS)); then
      CANDIDATE_MINIMUM=$PAIR_CANDIDATE_TIMING
      CANDIDATE_MINIMUM_MILLISECONDS=$PAIR_CANDIDATE_MILLISECONDS
    fi
  done

  SPEEDUP_THOUSANDTHS=$((
    REFERENCE_MAXIMUM_MILLISECONDS * 1000 /
    CANDIDATE_MINIMUM_MILLISECONDS
  ))
  printf "  %-16s%s maximum\n" "$REFERENCE_LABEL" "$REFERENCE_MAXIMUM"
  printf "  %-16s%s minimum\n" "$CANDIDATE_LABEL" "$CANDIDATE_MINIMUM"
  echo "output matches $REFERENCE_NAME"
  printf 'best-case speedup is %d.%03d\n' \
    "$((SPEEDUP_THOUSANDTHS / 1000))" \
    "$((SPEEDUP_THOUSANDTHS % 1000))"

  if ((REQUIRED_SPEEDUP > 0)); then
    if [ "$MODE" = sh ]; then
      if ((REFERENCE_MAXIMUM_MILLISECONDS <=
           CANDIDATE_MINIMUM_MILLISECONDS * REQUIRED_SPEEDUP)); then
        echo "shit is not faster than dash" >&2
        return 1
      fi
    else
      if ((REFERENCE_MAXIMUM_MILLISECONDS <
           CANDIDATE_MINIMUM_MILLISECONDS * REQUIRED_SPEEDUP)); then
        echo "shit is not at least $REQUIRED_SPEEDUP times faster" \
          "than bash" >&2
        return 1
      fi
    fi
  fi
}

require_executable "$DASH"
require_executable "$BASHP"

case "$BENCH_WARMUP_COUNT:$BENCH_SAMPLE_COUNT" in
  *[!0-9:]*|:*|*:)
    echo "benchmark sample counts must be nonnegative integers" >&2
    exit 1
    ;;
esac
if [ "$BENCH_SAMPLE_COUNT" -lt 1 ]; then
  echo "the benchmark sample count must be a positive integer" >&2
  exit 1
fi
case "$DASH_SPEEDUP_REQUIRED:$BASH_SPEEDUP_REQUIRED" in
  *[!0-9:]*|:*|*:)
    echo "benchmark speedup thresholds must be nonnegative integers" >&2
    exit 1
    ;;
esac

echo "configure.sh, wall-clock seconds at SCALE=$SCALE, lower is better:"
benchmark_pair sh
run_timed "$(basename "$BASHP")" "$B" "$BASHP" "$BENCH"
if command -v "$ZSH" >/dev/null 2>&1; then
  run_timed "$(basename "$ZSH")" "$Z" "$ZSH" --emulate sh "$BENCH"
else
  printf "  %-16s%s\n" "$(basename "$ZSH")" \
    "skipped, executable was not found"
fi
if command -v "$ASH" >/dev/null 2>&1; then
  run_timed "$(basename "$ASH") ash" "$G" "$ASH" ash "$BENCH"
else
  printf "  %-16s%s\n" "$(basename "$ASH") ash" \
    "skipped, executable was not found"
fi
if command -v "$YASH" >/dev/null 2>&1; then
  run_timed "$(basename "$YASH")" "$L" "$YASH" "$BENCH"
else
  printf "  %-16s%s\n" "$(basename "$YASH")" \
    "skipped, executable was not found"
fi
run_timed "$(basename "$BIN")+analysis" "$S" "$BIN" --mood sh -W "$BENCH"
compare_outputs "$D" "$S" "dash with analysis"

echo "configure.bash, wall-clock seconds at SCALE=$SCALE, lower is better:"
benchmark_pair bash
run_timed "$(basename "$BIN")+analysis" "$SB" \
  "$BIN" --mood bash -W "$BENCH_BASH"
compare_outputs "$BB" "$SB" "bash with analysis"

echo "configure.shit, wall-clock seconds at SCALE=$SCALE, lower is better:"
run_timed "$(basename "$BASHP")" "$ZB" "$BASHP" "$BENCH_SHIT"
run_timed "$(basename "$BIN")+analysis" "$ZS" "$BIN" "$BENCH_SHIT"
compare_outputs "$ZB" "$ZS" "bash with analysis"

echo "primes.bash, wall-clock seconds up to LIMIT=$PRIMES_LIMIT, lower is better:"
if command -v "$PYTHON" >/dev/null 2>&1; then
  run_timed "$(basename "$PYTHON")" "$PP" \
    "$PYTHON" "$PRIMES_PY" "$PRIMES_LIMIT"
else
  printf "  %-16s%s\n" "$(basename "$PYTHON")" \
    "skipped, executable was not found"
fi
run_timed "$(basename "$BASHP")" "$PB" "$BASHP" "$PRIMES" "$PRIMES_LIMIT"
run_timed "$(basename "$BIN")" "$PS" \
  "$BIN" --mood bash --no-diagnostics "$PRIMES" "$PRIMES_LIMIT"
compare_outputs "$PB" "$PS" "bash"
run_timed "$(basename "$BIN")+analysis" "$PS" \
  "$BIN" --mood bash -W "$PRIMES" "$PRIMES_LIMIT"
compare_outputs "$PB" "$PS" "bash with analysis"
if command -v "$PYTHON" >/dev/null 2>&1; then
  compare_outputs "$PB" "$PP" "python"
fi
