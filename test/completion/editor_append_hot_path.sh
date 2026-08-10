d=$(mktemp -d)
trap 'test -n "$d" && /bin/rm -rf "$d"' EXIT
script_command=$(command -v script)
expect_command=$(command -v expect || :)
RCFILE="$d/editor-rc"
export RCFILE
printf '%s\n' \
    "PROMPT_COMMAND='printf ready > \"\$EDITOR_READY_FILE\"; unset PROMPT_COMMAND'" \
    > "$RCFILE"

if "$script_command" --version >/dev/null 2>&1; then
    script_style=gnu
else
    script_style=bsd
fi

run_editor()
{
    transcript=$1
    columns=${2-80}
    script_error=$transcript.script-error
    recorder_status=0
    if [ "$script_style" = gnu ]; then
        "$script_command" -q -c \
            "/bin/stty cols $columns rows 24; exec \"\$BIN\" -i --rcfile \"\$RCFILE\"" \
            "$transcript" >/dev/null 2>"$script_error" || recorder_status=$?
    elif [ -n "$expect_command" ]; then
        TRANSCRIPT="$transcript" COLUMNS="$columns" "$expect_command" -c '
            set timeout 10
            log_user 0
            log_file -noappend $env(TRANSCRIPT)
            spawn -noecho /bin/sh -c "/bin/stty cols $env(COLUMNS) rows 24; exec \"$env(BIN)\" -i --rcfile \"$env(RCFILE)\""
            set editor_pid [exp_pid]
            set wall_timeout [after 30000 {
                puts stderr "editor recorder exceeded 30 seconds"
                catch {exec /bin/kill -KILL $editor_pid}
                close
                wait
                exit 1
            }]
            set editor_eof 0
            interact -nobuffer "\n" { after 200 } \
                -nobuffer "\003" { after 200 } timeout 10 {
                puts stderr "editor recorder input was idle for 10 seconds"
                catch {exec /bin/kill -KILL $editor_pid}
                close
                wait
                exit 1
            } eof { return } -o eof {
                set editor_eof 1
                return
            }
            if {!$editor_eof} {
                expect {
                    eof {}
                    timeout {
                        puts stderr "editor recorder output was idle for 10 seconds"
                        catch {exec /bin/kill -KILL $editor_pid}
                        close
                        wait
                        exit 1
                    }
                }
            }
            after cancel $wall_timeout
            set editor_wait [wait]
            if {[llength $editor_wait] != 4 || [lindex $editor_wait 2] != 0 || [lindex $editor_wait 3] != 0} {
                puts stderr "editor exited unsuccessfully: $editor_wait"
                exit 1
            }
        ' >/dev/null 2>"$script_error" || recorder_status=$?
    else
        "$script_command" -q /dev/null /bin/sh -c \
            "/bin/stty cols $columns rows 24; exec \"\$BIN\" -i --rcfile \"\$RCFILE\"" \
            >"$transcript" 2>"$script_error" || recorder_status=$?
    fi
    if [ "$recorder_status" -ne 0 ]; then
        [ ! -s "$script_error" ] || /bin/cat "$script_error"
        return "$recorder_status"
    fi

    return 0
}

wait_for_editor()
{
    ready_file=$1
    attempt_count=0
    while ! grep -F ready "$ready_file" >/dev/null 2>&1; do
        [ "$attempt_count" -lt 1000 ] || return 1
        sleep 0.01
        attempt_count=$((attempt_count + 1))
    done
}

wait_for_prompt_count()
{
    ready_file=$1
    expected_prompt_count=$2
    attempt_count=0
    while [ "$(grep '^ready$' "$ready_file" 2>/dev/null | wc -l | tr -d ' ')" \
        -lt "$expected_prompt_count" ]; do
        [ "$attempt_count" -lt 3000 ] || return 1
        sleep 0.01
        attempt_count=$((attempt_count + 1))
    done
}

metric_line()
{
    local metric_line_value
    metric_line_value=$(strings "$1" | grep 'editor-refresh append=' |
        sed -n "${2}p")
    [ -n "$metric_line_value" ] || return 1
    printf '%s\n' "$metric_line_value"
}

metric_field()
{
    metric=$1
    field=$2
    value=${metric#* "$field"=}
    printf '%s\n' "${value%% *}"
}

mkdir "$d/path"
printf '#!/bin/sh\n' > "$d/path/probe-alpha"
printf '#!/bin/sh\n' > "$d/path/probe-beta"
chmod +x "$d/path/probe-alpha" "$d/path/probe-beta"

send_typing_input()
{
    wait_for_editor "$d/typing-ready" || exit 1
    for character in e c h o ' ' h e l l o; do
        printf %s "$character"
        sleep 0.02
    done
    printf '\n'
    for character in p r o b e; do
        printf %s "$character"
        sleep 0.02
    done
    printf '\n'
    sleep 0.2
    printf 'exit 0\n'
    sleep 0.5
}

send_typing_input | TERM=xterm-256color PATH="$d/path" \
    KOSH_TEST_EDITOR_STATS=1 EDITOR_READY_FILE="$d/typing-ready" \
    KOSH_HISTORY="$d/typing-history" BIN="$BIN" \
    run_editor "$d/typing-typescript" || exit 1

append_metrics=$(metric_line "$d/typing-typescript" 1) || {
    printf 'editor metrics missing\n'
    strings "$d/typing-typescript" || true
    [ ! -s "$d/typing-typescript.script-error" ] ||
        /bin/cat "$d/typing-typescript.script-error"
    exit 1
}
append_refreshes=$(metric_field "$append_metrics" append)
full_refreshes=$(metric_field "$append_metrics" full)
append_serializations=$(metric_field "$append_metrics" serializations)
refresh_count=$((append_refreshes + full_refreshes))
if [ "$append_refreshes" -lt 6 ] || [ "$full_refreshes" -lt 1 ] ||
    [ "$refresh_count" -lt 11 ] || [ "$refresh_count" -gt 12 ] ||
    [ "$append_serializations" -gt 1 ]; then
    printf '%s\n' "$append_metrics"
    exit 1
fi
echo 'single-row typing uses incremental refresh'

path_metrics=$(metric_line "$d/typing-typescript" 2) || exit 1
test "$(metric_field "$path_metrics" stats)" -le 1 || exit 1
test "$(metric_field "$path_metrics" probes)" -le 2 || exit 1
test "$(metric_field "$path_metrics" sorts)" -eq 0 || exit 1
path_scan_count=$(metric_field "$path_metrics" scans)
append_scan_count=$(metric_field "$append_metrics" scans)
test "$((path_scan_count - append_scan_count))" -le 64 || exit 1
test "$(metric_field "$path_metrics" serializations)" -le 6 || exit 1
case $path_metrics in *' materialized=0'*) ;; *) exit 1 ;; esac
echo 'PATH ghost completion stays bounded while typing'

tab_unrelated_index=0
while [ "$tab_unrelated_index" -lt 32 ]; do
    printf '#!/bin/sh\n' > "$d/path/unrelated-command-$tab_unrelated_index"
    chmod +x "$d/path/unrelated-command-$tab_unrelated_index"
    tab_unrelated_index=$((tab_unrelated_index + 1))
done

send_tab_input()
{
    wait_for_prompt_count "$d/tab-ready" 1 || exit 1
    printf 'probe\talpha\n'
    wait_for_prompt_count "$d/tab-ready" 2 || exit 1
    printf 'compgen -c >/dev/null 2>&1; cd /\n'
    wait_for_prompt_count "$d/tab-ready" 3 || exit 1
    printf 'probe\t\t\n'
    wait_for_prompt_count "$d/tab-ready" 4 || exit 1
    printf 'exit 0\n'
}

printf '%s\n' \
    "PROMPT_COMMAND='printf \"ready\\\\n\" >> \"\$EDITOR_READY_FILE\"'" \
    > "$d/tab-rc"
send_tab_input | PATH="$d/path" KOSH_TEST_EDITOR_STATS=1 \
    EDITOR_READY_FILE="$d/tab-ready" KOSH_HISTORY="$d/tab-history" \
    RCFILE="$d/tab-rc" BIN="$BIN" run_editor "$d/tab-typescript" || exit 1

tab_metrics=$(metric_line "$d/tab-typescript" 1) || exit 1
test "$(metric_field "$tab_metrics" probes)" -le 4 || exit 1
echo 'TAB validation ends before the next key'

warm_metrics=$(metric_line "$d/tab-typescript" 3) || {
    printf 'warm TAB metrics missing\n'
    strings "$d/tab-typescript" || true
    [ ! -s "$d/tab-typescript.script-error" ] ||
        /bin/cat "$d/tab-typescript.script-error"
    exit 1
}
case $warm_metrics in
    *' preprompt-stats=0 preprompt-reads=0 preprompt-sorts=0 preprompt-probes=0 preprompt-resolutions=0 preprompt-history-loads=0 '*) ;;
    *) exit 1 ;;
esac
test "$(metric_field "$warm_metrics" stats)" -eq 0 || exit 1
test "$(metric_field "$warm_metrics" reads)" -eq 0 || exit 1
test "$(metric_field "$warm_metrics" sorts)" -eq 0 || exit 1
test "$(metric_field "$warm_metrics" probes)" -eq 0 || exit 1
test "$(metric_field "$warm_metrics" resolutions)" -eq 0 || exit 1
echo 'repeated warm TAB and absolute PATH perform no PATH work after cd'

history_index=0
while [ "$history_index" -lt 32 ]; do
    printf 'zzzz-invalid-history-command-%s\n' "$history_index"
    history_index=$((history_index + 1))
done > "$d/miss-history"

send_history_input()
{
    wait_for_editor "$d/history-ready" || exit 1
    printf 'zz\n'
    sleep 0.2
    printf 'zzzzzzz\n'
    sleep 0.2
    printf 'exit 0\n'
    sleep 0.5
}

send_history_input | TERM=xterm-256color PATH="$d/path" \
    EDITOR_READY_FILE="$d/history-ready" KOSH_TEST_EDITOR_STATS=1 \
    KOSH_HISTORY="$d/miss-history" BIN="$BIN" \
    run_editor "$d/history-typescript" || exit 1

history_short_metrics=$(metric_line "$d/history-typescript" 1) || exit 1
history_metrics=$(metric_line "$d/history-typescript" 2) || exit 1
history_entry_scan_count=$(metric_field "$history_metrics" history-scans)
test "$history_entry_scan_count" -ge 32 || exit 1
test "$history_entry_scan_count" -le 33 || exit 1
case $history_metrics in *' history-loads=0 '*) ;; *) exit 1 ;; esac
history_short_scan_count=$(metric_field "$history_short_metrics" scans)
history_scan_count=$(metric_field "$history_metrics" scans)
test "$history_scan_count" -le "$history_short_scan_count" || exit 1
test "$(metric_field "$history_metrics" resolutions)" -eq 0 || exit 1
echo 'rejected history prefixes scan once without walking PATH'

mkdir "$d/startup-before" "$d/startup-after"
printf '#!/bin/sh\n' > "$d/startup-after/git"
chmod +x "$d/startup-after/git"
printf '%s\n' \
    "PS1='> '" \
    "PROMPT_COMMAND='PATH=\"\$STARTUP_AFTER\"; printf ready > \"\$EDITOR_READY_FILE\"; unset PROMPT_COMMAND'" \
    > "$d/startup-rc"

send_startup_input()
{
    wait_for_editor "$d/startup-ready" || exit 1
    printf 'git \177z\003'
    sleep 0.2
    printf 'exit 0\n'
    sleep 0.5
}

send_startup_input | TERM=xterm-256color NO_COLOR= PATH="$d/startup-before" \
    EDITOR_READY_FILE="$d/startup-ready" STARTUP_AFTER="$d/startup-after" \
    KOSH_TEST_EDITOR_STATS=1 KOSH_HISTORY="$d/startup-history" \
    RCFILE="$d/startup-rc" BIN="$BIN" run_editor "$d/startup-typescript" || exit 1

startup_metrics=$(metric_line "$d/startup-typescript" 1) || exit 1
test "$(metric_field "$startup_metrics" stats)" -eq 0 || exit 1
test "$(metric_field "$startup_metrics" reads)" -eq 0 || exit 1
test "$(metric_field "$startup_metrics" probes)" -eq 0 || exit 1
test "$(metric_field "$startup_metrics" sorts)" -eq 0 || exit 1
test "$(metric_field "$startup_metrics" resolutions)" -le 1 || exit 1
echo 'prompt PATH changes defer work while startup commands highlight'

mkdir "$d/menu-bin"
cat > "$d/menu-bin/tailscale" <<'SH'
#!/bin/sh
printf '%s\n' \
    'SUBCOMMANDS' \
    '  alpha        Keep this first long completion description intact' \
    '  beta         Keep this second long completion description intact'
SH
chmod +x "$d/menu-bin/tailscale"

send_menu_input()
{
    wait_for_editor "$d/menu-ready" || exit 1
    printf 'tailscale \t'
    sleep 0.5
    printf '\003'
    sleep 0.2
    printf 'exit 0\n'
    sleep 0.5
}

send_menu_input | ASAN_OPTIONS=detect_stack_use_after_return=1 \
    EDITOR_READY_FILE="$d/menu-ready" MANPATH= \
    PATH="$d/menu-bin${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
    KOSH_HISTORY="$d/menu-history" BIN="$BIN" \
    run_editor "$d/menu-typescript" 100 || exit 1

strings "$d/menu-typescript" | \
    grep -q 'Keep this first long completion description intact' || exit 1
strings "$d/menu-typescript" | \
    grep -q 'Keep this second long completion description intact' || exit 1
echo 'completion menu keeps callback-owned strings alive'

mkdir "$d/quoted-completion"
touch "$d/quoted-completion/space name" \
    "$d/quoted-completion/plain name" \
    "$d/quoted-completion/README-one" \
    "$d/quoted-completion/ReadMe-two"

send_quoted_input()
{
    wait_for_editor "$d/quoted-ready" || exit 1
    printf "cd '%s'\n" "$d/quoted-completion"
    sleep 0.2
    printf "printf '<%%s>\\\\n' 'spX'\033[D\033[D\t\n"
    sleep 0.2
    printf "printf '<%%s>\\\\n' plaX\033[D\t\n"
    sleep 0.2
    printf "COMPLETION_PROBE=variable-value\n"
    sleep 0.2
    printf "printf '<%%s>\\\\n' \"\$COMPLETION_PROBX\"\033[D\033[D\t\n"
    sleep 0.2
    printf "printf '<%%s>\\\\n' read\tone\n"
    sleep 0.2
    printf 'exit 0\n'
    sleep 0.5
}

send_quoted_input | EDITOR_READY_FILE="$d/quoted-ready" \
    KOSH_HISTORY="$d/quoted-history" BIN="$BIN" \
    run_editor "$d/quoted-typescript" || exit 1

for quoted_expected_output in \
    '<space name>' '<plain name>' '<variable-value>' '<README-one>'
do
    strings "$d/quoted-typescript" | grep -q "$quoted_expected_output" || {
        printf 'quoted completion output missing: %s\n' "$quoted_expected_output"
        strings "$d/quoted-typescript" || true
        [ ! -s "$d/quoted-typescript.script-error" ] ||
            /bin/cat "$d/quoted-typescript.script-error"
        exit 1
    }
done
echo 'quoted replacement and smart-case TAB preserve the completed token'

send_navigation_input()
{
    wait_for_prompt_count "$d/navigation-ready" 1 || exit 1
    printf 'echo alt-left-XXX\033[1;3Dfixed-\n'
    wait_for_prompt_count "$d/navigation-ready" 2 || exit 1
    printf 'echo ctrl-left-XXX\033[1;5Dfixed-\n'
    wait_for_prompt_count "$d/navigation-ready" 3 || exit 1
    printf 'echo XXX tail\001\033[C\033[C\033[C\033[C\033[C\033[1;3C-fixed\n'
    wait_for_prompt_count "$d/navigation-ready" 4 || exit 1
    printf 'echo XXX tail\001\033[C\033[C\033[C\033[C\033[C\033[1;5C-fixed\n'
    wait_for_prompt_count "$d/navigation-ready" 5 || exit 1
    printf 'cho command-home\033[He\n'
    wait_for_prompt_count "$d/navigation-ready" 6 || exit 1
    printf 'echo command-end\033[H\033[F-ok\n'
    wait_for_prompt_count "$d/navigation-ready" 7 || exit 1
    printf 'echo option-del remove\033\177kept\n'
    wait_for_prompt_count "$d/navigation-ready" 8 || exit 1
    printf 'echo option-bs remove\033\010kept\n'
    wait_for_prompt_count "$d/navigation-ready" 9 || exit 1
    printf 'echo ctrl-w remove\027kept\n'
    wait_for_prompt_count "$d/navigation-ready" 10 || exit 1
    printf 'exit 0\n'
}

printf '%s\n' \
    "PROMPT_COMMAND='printf \"ready\\\\n\" >> \"\$EDITOR_READY_FILE\"'" \
    > "$d/navigation-rc"
send_navigation_input | TERM=xterm-256color \
    EDITOR_READY_FILE="$d/navigation-ready" \
    KOSH_HISTORY="$d/navigation-history" RCFILE="$d/navigation-rc" BIN="$BIN" \
    run_editor "$d/navigation-typescript" || exit 1

for navigation_expected_output in \
    'alt-left-fixed-XXX' 'ctrl-left-fixed-XXX' \
    'XXX-fixed tail' 'command-home' 'command-end-ok' \
    'option-del kept' 'option-bs kept' 'ctrl-w kept'
do
    strings "$d/navigation-typescript" | \
        grep -q "$navigation_expected_output" || exit 1
done
echo 'Alt and Ctrl keys preserve word and line editing'

mkdir "$d/mixed-path" "$d/next-directory"
printf '#!/bin/sh\n' > "$d/mixed-path/after-cd-probe"
chmod +x "$d/mixed-path/after-cd-probe"

send_mixed_path_input()
{
    wait_for_editor "$d/mixed-ready" || exit 1
    printf 'cd %s\n' "$d/next-directory"
    sleep 0.2
    printf 'after-cd-probe\014\n'
    sleep 0.2
    printf 'exit 0\n'
    sleep 0.5
}

unset NO_COLOR
send_mixed_path_input | TERM=xterm-256color \
    EDITOR_READY_FILE="$d/mixed-ready" \
    PATH="$d/mixed-path${TEST_PATH_SEPARATOR}${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
    KOSH_TEST_EDITOR_STATS=1 KOSH_HISTORY="$d/mixed-history" BIN="$BIN" \
    run_editor "$d/mixed-typescript" || exit 1

mixed_metrics=$(metric_line "$d/mixed-typescript" 2) || exit 1
case $mixed_metrics in *' stats=0 reads=0 sorts=0 probes=0 '*) ;; *) exit 1 ;; esac
echo 'mixed PATH keeps stale absolute commands highlighted after cd'

printf '#!/bin/sh\nprintf "actual-cwd-completion\\n"\n' > "$d/actual-cwd-probe"
chmod +x "$d/actual-cwd-probe"
actual_cwd_completion=$(
    cd "$d" && PWD=invalid "$BIN" --debug-complete-at './actual-cwd-'
)
printf '%s\n' "$actual_cwd_completion" | grep -q actual-cwd-probe || exit 1
echo 'clobbered PWD completion uses the actual directory'
