#!/bin/bash
# Benchmark configure.sh, configure.bash, and configure.shit across the reference
# shells and shit, reporting wall-clock seconds at the given scale and checking
# that shit output matches the reference shell. The Makefile passes SCALE, BIN,
# DASH, BASHP, ZSH, ASH, YASH, BENCH, BENCH_BASH, and BENCH_SHIT. Run from the
# test directory. The bash time keyword formats the wall clock through
# TIMEFORMAT.

export TIMEFORMAT="  %R"

WORK=$TEST_TEMP_DIRECTORY/bench
rm -rf "$WORK"
mkdir -p "$WORK"
trap 'rm -rf "$WORK"' EXIT
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

run_ref() {
  if ! command -v "$1" >/dev/null; then return 0; fi
  printf "  %-16s" "${2:-$1}"
  ( time SCALE=$SCALE "$1" $3 >"$4" 2>&1 ) 2>&1
}

compare() {
  if cmp "$1" "$2"; then echo "output matches $3"
  else echo "output differs from $3:"; diff "$1" "$2" | head -20; fi
}

echo "configure.sh, wall-clock seconds at SCALE=$SCALE, lower is better:"
run_ref "$DASH"  "$(basename "$DASH")"  "$BENCH" "$D"
run_ref "$BASHP" "$(basename "$BASHP")" "$BENCH" "$B"
run_ref "$ZSH"   "$(basename "$ZSH")"   "$BENCH" "$Z"
run_ref "$ASH"   "$(basename "$ASH")"   "$BENCH" "$G"
run_ref "$YASH"  "$(basename "$YASH")"  "$BENCH" "$L"
printf "  %-16s" "$(basename "$BIN")";  ( time SCALE=$SCALE $BIN --mood sh $BENCH >"$S" 2>&1 ) 2>&1
compare "$D" "$S" "dash"
printf "  %-16s" "$(basename "$BIN")+analysis"; ( time SCALE=$SCALE $BIN --mood sh -W $BENCH >"$S" 2>/dev/null ) 2>&1
compare "$D" "$S" "dash with analysis"

echo "configure.bash, wall-clock seconds at SCALE=$SCALE, lower is better:"
printf "  %-16s" "$(basename "$BASHP")"; ( time SCALE=$SCALE $BASHP $BENCH_BASH >"$BB" 2>&1 ) 2>&1
printf "  %-16s" "$(basename "$BIN")";  ( time SCALE=$SCALE $BIN --mood bash $BENCH_BASH >"$SB" 2>&1 ) 2>&1
compare "$BB" "$SB" "bash"
printf "  %-16s" "$(basename "$BIN")+analysis"; ( time SCALE=$SCALE $BIN --mood bash -W $BENCH_BASH >"$SB" 2>/dev/null ) 2>&1
compare "$BB" "$SB" "bash with analysis"

echo "configure.shit, wall-clock seconds at SCALE=$SCALE, lower is better:"
printf "  %-16s" "$(basename "$BASHP")"; ( time SCALE=$SCALE $BASHP $BENCH_SHIT >"$ZB" 2>&1 ) 2>&1
printf "  %-16s" "$(basename "$BIN")+analysis"; ( time SCALE=$SCALE $BIN $BENCH_SHIT >"$ZS" 2>/dev/null ) 2>&1
compare "$ZB" "$ZS" "bash with analysis"

echo "primes.bash, wall-clock seconds up to LIMIT=$PRIMES_LIMIT, lower is better:"
printf "  %-16s" "python3"; ( time python3 $PRIMES_PY $PRIMES_LIMIT >"$PP" 2>&1 ) 2>&1
printf "  %-16s" "$(basename "$BASHP")"; ( time $BASHP $PRIMES $PRIMES_LIMIT >"$PB" 2>&1 ) 2>&1
printf "  %-16s" "$(basename "$BIN")";  ( time $BIN --mood bash $PRIMES $PRIMES_LIMIT >"$PS" 2>&1 ) 2>&1
compare "$PB" "$PS" "bash"
printf "  %-16s" "$(basename "$BIN")+analysis"; ( time $BIN --mood bash -W $PRIMES $PRIMES_LIMIT >"$PS" 2>/dev/null ) 2>&1
compare "$PB" "$PS" "bash with analysis"
compare "$PB" "$PP" "python"
