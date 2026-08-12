unset KOSH_FLAGS
export KOSH_FLAGS=--no-diagnostics
d=$(mktemp -d)
trap 'test -n "$d" && /bin/rm -rf "$d"' EXIT
if [ "${OS-}" = Windows_NT ]; then
    BENCH_ECHO='cmd.exe /d /c echo'
else
    BENCH_ECHO=/bin/echo
fi
export BENCH_ECHO
# bench forks a kosh process per sample and runs the command under it. Only the
# deterministic lines are checked here, since the timing summary varies per run.
echo "== bench stops on a non-zero exit code:"
"$BIN" -c 'bench --runs 5 false' 2>&1 | grep -E 'exited with status|note:'
echo "== the failure sets the exit status:"
"$BIN" -c 'bench --runs 5 false' >/dev/null 2>&1; echo "rc=$?"
echo "== --ignore-exit-code keeps sampling a failing command:"
"$BIN" -c 'bench --runs 3 --ignore-exit-code false' 2>&1 | grep 'Benchmark:'
echo "== --no-shell --show-output forks the command directly:"
direct_output=$("$BIN" -c \
    'bench --runs 2 --no-shell --show-output "$BENCH_ECHO direct"' 2>&1)
if [ "${OS-}" = Windows_NT ]; then
    direct_output=$(printf '%s\n' "$direct_output" | tr -d '\r')
fi
printf '%s\n' "$direct_output" |
    sed "s|$BENCH_ECHO|/bin/echo|g" |
    grep -E '^direct$|Benchmark:'
echo "== --runs executes exactly one sample:"
"$BIN" -c 'bench --runs 1 --ignore-exit-code true' 2>&1 | grep 'Benchmark:'
echo "== invalid sample counts are rejected:"
for count in 0 100001 18446744073709551616; do
    "$BIN" -c "bench --runs '$count' true" >/dev/null 2>&1
    echo "rc=$?"
done
echo "== unsigned overflow is rejected by number parsing:"
"$BIN" -c 'bench --runs 18446744073709551616 true' 2>&1 |
    grep 'expects a number, got'
echo "== invalid values carry granular carets:"
"$BIN" -c 'bench --runs nope true' 2>&1
"$BIN" -c 'bench --runs=nope true' 2>&1
"$BIN" -c 'bench --runs 0 true' 2>&1
"$BIN" -c 'bench --duration 18446744073710 true' 2>&1
"$BIN" -c 'bench "--runs=nope" true' 2>&1
"$BIN" -c 'runs=nope; bench --runs=$runs true' 2>&1
echo "== an overflowing duration is rejected:"
"$BIN" -c 'bench --duration 18446744073710 true' >/dev/null 2>&1
echo "rc=$?"
echo "== counter capability keeps one complete sample:"
counter_output=$("$BIN" -c \
    'bench --runs 1 --no-shell --show-output "$BENCH_ECHO counter-run"' 2>&1)
if [ "${OS-}" = Windows_NT ]; then
    counter_output=$(printf '%s\n' "$counter_output" | tr -d '\r')
fi
test "$(printf '%s\n' "$counter_output" | grep -c '^counter-run$')" -eq 1
counter_row_count=$(printf '%s\n' "$counter_output" | grep -Ec '^  (cpu cycles|instructions|cache refs|cache misses|branch misses)')
case "$counter_row_count" in
    0) ;;
    5)
        cpu_cycle_row=$(printf '%s\n' "$counter_output" | grep '^  cpu cycles')
        instruction_row=$(printf '%s\n' "$counter_output" | grep '^  instructions')
        case "$cpu_cycle_row" in *[1-9]*) ;; *) exit 1 ;; esac
        case "$instruction_row" in *[1-9]*) ;; *) exit 1 ;; esac
        ;;
    *) exit 1 ;;
esac
if [ "$(uname -s)" = Linux ] && command -v perf >/dev/null 2>&1 &&
    perf stat -e cycles,instructions,cache-references,cache-misses,branch-misses \
        -- /bin/true >/dev/null 2>&1; then
    test "$counter_row_count" -eq 5
fi
case "$counter_output" in
    *"Benchmark: $BENCH_ECHO counter-run (1 runs)"*) ;;
    *) exit 1 ;;
esac
echo "counter capability passed"
echo "== counter fallback keeps one complete sample:"
if [ "$(uname -s)" = Linux ]; then
    fallback_output=$(
        "$BIN" -c \
            'ulimit -n 8; bench --runs 1 --no-shell --show-output "$BENCH_ECHO fallback-run"' \
            2>&1
    )
    test "$(printf '%s\n' "$fallback_output" | grep -c '^fallback-run$')" -eq 1
    test "$(printf '%s\n' "$fallback_output" | grep -Ec '^  (cpu cycles|instructions|cache refs|cache misses|branch misses)')" -eq 0
    case "$fallback_output" in
        *"Benchmark: $BENCH_ECHO fallback-run (1 runs)"*) ;;
        *) exit 1 ;;
    esac
fi
echo "counter fallback passed"
echo "== a failed later sample clears terminal progress:"
cat > "$d/vanishing-environment" <<'SH'
koshkit sleep 0.03 || exit 1
rm -f "$BENCH_VANISHING_COMMAND" || exit 1
SH
vanishing_command="$d/vanishing-command.exe"
ln -s "$BIN" "$vanishing_command"
has_typescript=0
has_progress_terminal=0
if script -qec true /dev/null >/dev/null 2>&1; then
    has_typescript=1
    if BIN="$BIN" script -qec \
        'exec "$BIN" -c "test -t 2"' /dev/null >/dev/null 2>&1; then
        has_progress_terminal=1
    fi
    NO_COLOR= TERM=xterm BIN=$BIN BASH_ENV="$d/vanishing-environment" \
        BENCH_VANISHING_COMMAND="$vanishing_command" script -qec \
        "$BIN -c 'bench --runs 3 --no-shell \"$vanishing_command --mood bash -c true\"'" \
        "$d/typescript" >/dev/null 2>&1
elif script -q /dev/null /usr/bin/true >/dev/null 2>&1; then
    has_typescript=1
    native_terminal_output=$(BIN="$BIN" script -q /dev/null "$BIN" -c \
        'test -t 2 && echo native-stderr-terminal' 2>/dev/null)
    case $native_terminal_output in
        *native-stderr-terminal*) has_progress_terminal=1 ;;
    esac
    NO_COLOR= TERM=xterm BIN=$BIN BASH_ENV="$d/vanishing-environment" \
        BENCH_VANISHING_COMMAND="$vanishing_command" \
        script -q "$d/typescript" "$BIN" -c \
        "bench --runs 3 --no-shell \"$vanishing_command --mood bash -c true\"" \
        >/dev/null 2>&1
else
    : > "$d/typescript"
fi
terminal_hex=$(od -An -tx1 "$d/typescript" | tr -d ' \n')
if [ "$has_typescript" -eq 1 ]; then
    [ -n "$terminal_hex" ] || exit 1
    [ ! -e "$vanishing_command" ] && [ ! -L "$vanishing_command" ] || exit 1
    typescript_text=$(tr -d '\r' < "$d/typescript")
    if [ "${OS-}" = Windows_NT ]; then
        case "$typescript_text" in
            *"Unable to run '$vanishing_command --mood bash -c true'"*) ;;
            *) exit 1 ;;
        esac
    else
        vanishing_row=$(printf '%s\n' "$typescript_text" |
            grep -F "$vanishing_command --mood bash -c true")
        case "$vanishing_row" in
            *'exited with status 127.'*) ;;
            *) exit 1 ;;
        esac
    fi
fi
if [ "$has_progress_terminal" -eq 1 ]; then
    case $terminal_hex in
        *0d1b5b324b1b5b316d*) ;;
        *) echo "progress clobbered error"; exit 1 ;;
    esac
fi
if [ "$has_progress_terminal" -eq 1 ]; then
    cat > "$d/progress-driver" <<'SH'
#!/bin/sh
exec "$BIN" -c 'bench --runs 2 "sleep 0.12 # deliberately long benchmark label" "sleep 0.12"'
SH
    chmod +x "$d/progress-driver"
    if script -qec true /dev/null >/dev/null 2>&1; then
        NO_COLOR= TERM=xterm BIN=$BIN script -qec "$d/progress-driver" \
            "$d/progress-typescript" >/dev/null 2>&1
    elif script -q /dev/null /usr/bin/true >/dev/null 2>&1; then
        NO_COLOR= TERM=xterm BIN=$BIN script -q "$d/progress-typescript" \
            "$d/progress-driver" >/dev/null 2>&1
    fi
    progress_hex=$(od -An -tx1 "$d/progress-typescript" | tr -d ' \n')
    clear_count=$(printf '%s\n' "$progress_hex" | grep -o '0d1b5b324b' | \
        wc -l | tr -d ' ')
    [ "$clear_count" -ge 4 ] || exit 1
fi
echo "progress clear check complete"
