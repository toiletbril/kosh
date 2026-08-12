unset KOSH_FLAGS

d=$(mktemp -d)
d=$(cd "$d" && pwd -P)
trap 'test -n "$d" && /bin/rm -rf "$d"' EXIT

probe="$d/stopped.bash"
printf '%s\n' '#!/bin/bash' \
    'kill -STOP "$BASHPID"' \
    'echo FG_READY' \
    'read value' \
    'echo "FG_READ:$value"' > "$probe"
chmod +x "$probe"

printf 'printf ready > "%s"\nPROMPT_COMMAND='\''cd "%s"; printf "\\033]0;HOOK\\007"'\''\n' \
    "$d/ready" "$d" > "$d/rc"

wait_for_transcript()
{
    text=$1
    attempt_count=0
    while ! grep -aF "$text" "$d/live" >/dev/null 2>&1; do
        [ "$attempt_count" -lt 1000 ] || return 1
        sleep 0.01
        attempt_count=$((attempt_count + 1))
    done
}

send_input()
{
    attempt_count=0
    while ! grep -F ready "$d/ready" >/dev/null 2>&1; do
        [ "$attempt_count" -lt 1000 ] || return 1
        sleep 0.01
        attempt_count=$((attempt_count + 1))
    done
    wait_for_transcript 'Bash me harder!' || return 1
    printf 'set --mood kosh; echo HANDOFF_STEP_1\n'
    wait_for_transcript 'HANDOFF_STEP_1' || return 1
    printf 'stty tostop; echo HANDOFF_STEP_2\n'
    wait_for_transcript 'HANDOFF_STEP_2' || return 1
    printf '%s\n' "( /usr/bin/true \$'safe\\001\\302\\200\\303\\251' ) > redirected"
    printf "%s 'argument with spaces'\n" "$probe"
    wait_for_transcript 'Stopped' || return 1
    printf 'fg\n'
    wait_for_transcript 'FG_READY' || return 1
    printf 'terminal-value\n'
    wait_for_transcript 'FG_READ:terminal-value' || return 1
    printf 'exit\n'
}

if script -q -c true /dev/null >/dev/null 2>&1; then
    send_input | script -q -c \
        "exec \"$BIN\" -i -I -X debug --debug-logging-file \"$d/log\" --mood bash --rcfile \"$d/rc\"" \
        "$d/typescript" >"$d/live" 2>/dev/null
elif script -q /dev/null /usr/bin/true >/dev/null 2>&1; then
    send_input | script -q "$d/typescript" /bin/sh -c \
        "exec \"$BIN\" -i -I -X debug --debug-logging-file \"$d/log\" --mood bash --rcfile \"$d/rc\"" \
        >"$d/live" 2>/dev/null
else
    exit 1
fi

output=$(strings "$d/typescript")
ordering=failed
if grep -q 'fg will give the terminal.*before it resumes job' "$d/log"; then
    ordering=passed
fi

title_output=failed
escape=$(printf '\033')
bell=$(printf '\007')
if [ -n "${LOGNAME+x}" ]; then
    expected_user=$LOGNAME
elif [ -n "${USER+x}" ]; then
    expected_user=$USER
else
    expected_user=$(id -un)
fi
safe_argument=$(printf 'safe\303\251')
idle_title="${escape}]0;${expected_user} @ ${d}${bell}"
probe_title="${escape}]0;${probe} 'argument with spaces'${bell}"
sanitized_title="${escape}]0;/usr/bin/true ${safe_argument}${bell}"
hook_title="${escape}]0;HOOK${bell}"
probe_title_count=$(grep -aoF "$probe_title" "$d/typescript" 2>/dev/null |
    wc -l | tr -d ' ')
last_probe_position=$(grep -aboF "$probe_title" "$d/typescript" 2>/dev/null |
    tail -n 1 | cut -d: -f1)
last_idle_position=$(grep -aboF "$idle_title" "$d/typescript" 2>/dev/null |
    tail -n 1 | cut -d: -f1)
last_hook_position=$(grep -aboF "$hook_title" "$d/typescript" 2>/dev/null |
    tail -n 1 | cut -d: -f1)
if grep -aF "$sanitized_title" "$d/typescript" >/dev/null 2>&1 &&
    [ "$probe_title_count" -ge 2 ] && [ -n "$last_probe_position" ] &&
    [ -n "$last_idle_position" ] && [ -n "$last_hook_position" ] &&
    [ "$last_idle_position" -gt "$last_probe_position" ] &&
    [ "$last_hook_position" -gt "$last_idle_position" ]; then
    title_output=passed
fi

case "$ordering:$title_output:$output" in
    passed:passed:*FG_READ:terminal-value*) echo passed ;;
    *) grep 'fg ' "$d/log"; printf '%s\n' "$output"; echo failed ;;
esac
