#!/bin/sh

unset KOSH_FLAGS
export KOSH_FLAGS=--no-diagnostics
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")

d=$(mktemp -d) || exit 1
query_reader_pid=
descendant_pid=
descriptor_pid=
preserve_pid=
supervisor_pid=
watchdog_pid=
host_system=$(uname -s)
process_is_alive()
{
    process_id=$1
    kill -0 "$process_id" 2>/dev/null || return 1
    if [ "$host_system" = Linux ]; then
        process_stat_path=/proc/$process_id/stat
        [ -r "$process_stat_path" ] || return 1
        IFS= read -r process_stat < "$process_stat_path" || return 1
        process_fields=${process_stat##*) }
        set -- $process_fields
        [ "$#" -ge 1 ] || return 1
        [ "$1" != Z ]
        return
    fi
    return 0
}
cleanup()
{
    for cleanup_pid in "$query_reader_pid" "$descendant_pid" "$descriptor_pid" \
        "$preserve_pid" "$supervisor_pid" "$watchdog_pid"; do
        if [ -n "$cleanup_pid" ]; then
            kill -KILL "$cleanup_pid" 2>/dev/null || true
            wait "$cleanup_pid" 2>/dev/null || true
        fi
    done
    if [ -n "$d" ]; then
        rm -rf "$d"
    fi
}
trap cleanup EXIT

if [ "${OS-}" = Windows_NT ]; then
    mkdir "$d/noexec"
else
    printf '#!/bin/sh\nexit 0\n' > "$d/noexec"
    chmod 644 "$d/noexec"
fi
printf 'echo fallback-script\n' > "$d/plain-script"
printf 'finish()\n{\n  exit 23\n}\ntrap finish TERM\nprintf "%%s\n" "$$" > "$2"\nprintf armed > "$1"\nkill -STOP "$$"\nwhile :; do :; done\n' \
    > "$d/preserve.sh"
printf 'stopped_result=$1\nstopped_ready=$2\nterm_received=no\nresume()\n{\n  term_received=yes\n}\ntrap resume TERM\nprintf armed > "$stopped_ready"\nkill -STOP "$$"\nprintf resumed > "$stopped_result"\nwhile [ "$term_received" = no ]; do :; done\n' \
    > "$d/stopped.sh"
printf '(printf ready > "$1"; exec /bin/sleep 10) &\ndescendant_pid=$!\nprintf "%%s\\n" "$descendant_pid" > "$2"\nwhile [ ! -s "$1" ]; do :; done\nprintf ready > "$3"\nwait "$descendant_pid"\n' \
    > "$d/descendant.sh"
printf 'finish()\n{\n  exit 0\n}\ntrap finish TERM\n(trap "" TERM; printf "descriptor-ready\\n" >&3; printf ready > "$1"; exec /bin/sleep 10) &\ndescendant_pid=$!\nprintf "%%s\\n" "$descendant_pid" > "$2"\nwhile [ ! -s "$1" ]; do :; done\nprintf ready > "$3"\nwait "$descendant_pid"\n' \
    > "$d/descriptor.sh"
printf '(sleep 10) &\ndescendant_pid=$!\nprintf "%%s\\n" "$descendant_pid" > "$1"\nprintf ready > "$2"\nwait "$descendant_pid"\n' \
    > "$d/windows-descendant.sh"
printf '(sleep 10) &\ndescendant_pid=$!\nprintf "%%s\\n" "$descendant_pid" > "$1"\nprintf "descriptor-ready\\n"\nwait "$descendant_pid"\n' \
    > "$d/windows-descriptor.sh"
chmod +x "$d/plain-script"

echo "--- timeout passes output and status ---"
"$BIN" -c 'koshkit timeout 1s "$1" -c "echo okay"' \
    timeout-child "$BIN"
"$BIN" -c 'koshkit timeout 1s "$1" -c "exit 7"' timeout-child "$BIN"
echo "rc=$?"

echo "--- timeout returns its timeout status ---"
"$BIN" -c 'koshkit timeout 0.01s "$1" -c "koshkit sleep 1"' \
    timeout-child "$BIN"
echo "rc=$?"

echo "--- timeout preserves the selected signal status ---"
if [ "${OS-}" = Windows_NT ]; then
    "$BIN" -c \
        'koshkit timeout -p -s TERM 0.02s "$1" -c "koshkit sleep 1"' \
        timeout-child "$BIN" \
        >/dev/null 2>&1
else
    "$BIN" -c \
        "koshkit timeout -p -s TERM -k 1s 1s /bin/sh '$d/preserve.sh' '$d/preserve-ready' '$d/preserve-pid'" \
        >/dev/null 2>&1
fi
preserved_status=$?
if [ -s "$d/preserve-pid" ]; then
    preserve_pid=$(cat "$d/preserve-pid")
fi
preserve_ready=missing
if [ -s "$d/preserve-ready" ]; then
    preserve_ready=$(cat "$d/preserve-ready")
fi
preserve_is_alive=no
if [ -n "$preserve_pid" ] && process_is_alive "$preserve_pid"; then
    preserve_is_alive=yes
fi
if { [ "${OS-}" = Windows_NT ] && [ "$preserved_status" -eq 1 ]; } ||
    { [ "${OS-}" != Windows_NT ] && [ "$preserved_status" -eq 23 ] &&
      [ "$preserve_ready" = armed ] && [ "$preserve_is_alive" = no ]; }; then
    echo passed
else
    echo "failed=$preserved_status"
fi
if [ "$preserve_is_alive" = yes ]; then
    kill -KILL "$preserve_pid" 2>/dev/null || true
    wait "$preserve_pid" 2>/dev/null || true
fi
preserve_pid=

echo "--- KILL timeout always reports signal status ---"
for kill_options in '-s KILL' '-p -s KILL' '-k 1s -s KILL' '-p -k 1s -s KILL'; do
    KILL_OPTIONS="$kill_options" "$BIN" -c \
        'koshkit timeout $KILL_OPTIONS 0.02s "$1" -c "koshkit sleep 1"' \
        timeout-child "$BIN" \
        >/dev/null 2>&1
    echo "rc=$?"
done

echo "--- timeout forces a kill after the grace period ---"
if [ "${OS-}" = Windows_NT ]; then
    "$BIN" -c \
        'koshkit timeout -s 0 -k 0.02s 0.02s "$1" -c "koshkit sleep 1"' \
        timeout-child "$BIN" \
        >/dev/null 2>&1
else
    "$BIN" -c 'koshkit timeout -s 0 -k 0.02s 0.02s /bin/sleep 1' \
        >/dev/null 2>&1
fi
kill_after_status=$?
if [ "$kill_after_status" -eq 137 ]; then
    echo passed
else
    echo "failed=$kill_after_status"
fi

echo "--- timeout zero allows completion ---"
"$BIN" -c 'koshkit timeout 0 "$1" -c "exit 9"' timeout-child "$BIN"
echo "rc=$?"

echo "--- positive sub-nanosecond limits still expire ---"
"$BIN" -c \
    'koshkit timeout 0.0000000001 "$1" -c "koshkit sleep 0.2"' \
    timeout-child "$BIN" >/dev/null 2>&1
echo "rc=$?"
if [ "${OS-}" = Windows_NT ]; then
    echo kill-after-passed
else
    "$BIN" -c 'koshkit timeout -s 0 -k 0.0000000001 0.0000000001 /bin/sleep 0.2' \
        >/dev/null 2>&1
    subnanosecond_kill_status=$?
    if [ "$subnanosecond_kill_status" -eq 137 ]; then
        echo kill-after-passed
    else
        echo "kill-after-failed=$subnanosecond_kill_status"
    fi
fi

echo "--- timeout reports missing and unusable commands ---"
"$BIN" -c 'koshkit timeout 1 no_such_timeout_command_xyz' >/dev/null 2>&1
echo "missing=$?"
"$BIN" -c "koshkit timeout 1 '$d/noexec'" >/dev/null 2>&1
echo "unusable=$?"
"$BIN" -c "koshkit timeout 1 '$d/plain-script'"

echo "--- timeout rejects unsupported signals ---"
"$BIN" -c 'koshkit timeout -s 999 1 "$1" -c true' \
    timeout-child "$BIN" >/dev/null 2>&1
echo "rc=$?"
"$BIN" -c 'koshkit timeout -s 4294967311 1 "$1" -c true' \
    timeout-child "$BIN" >/dev/null 2>&1
echo "wrapped=$?"

echo "--- timeout resumes a stopped child to finish supervision ---"
if [ "${OS-}" = Windows_NT ]; then
    echo rc=124
else
    "$BIN" -c "koshkit timeout -k 0.2s 1s /bin/sh '$d/stopped.sh' '$d/stopped-result' '$d/stopped-ready'" \
        >/dev/null 2>&1
    stopped_status=$?
    stopped_result=missing
    if [ -s "$d/stopped-result" ]; then
        stopped_result=$(cat "$d/stopped-result")
    fi
    stopped_ready=missing
    if [ -s "$d/stopped-ready" ]; then
        stopped_ready=$(cat "$d/stopped-ready")
    fi
    if [ "$stopped_status" -eq 124 ] && [ "$stopped_ready" = armed ] &&
        [ "$stopped_result" = resumed ]; then
        echo rc=124
    else
        echo "failed=$stopped_status ready=$stopped_ready result=$stopped_result"
    fi
fi

echo "--- timeout rejects invalid durations ---"
for duration in abc -1 nan 0x1 1q; do
    DURATION="$duration" "$BIN" -c \
        'koshkit timeout "$DURATION" "$1" -c true' \
        timeout-child "$BIN" >/dev/null 2>&1
    echo "invalid=$?"
done

echo "--- timeout kills process group descendants ---"
if [ "${OS-}" = Windows_NT ]; then
    "$BIN" -c "koshkit timeout 1s sh '$d/windows-descendant.sh' '$d/descendant-pid' '$d/descendant-ready'" \
        >"${TEST_NULL_DEVICE:-/dev/null}" 2>&1
    descendant_status=$?
    descendant_pid=
    if [ -s "$d/descendant-pid" ]; then
        descendant_pid=$(cat "$d/descendant-pid")
    fi
    waited=0
    while [ -n "$descendant_pid" ] && process_is_alive "$descendant_pid" &&
        [ "$waited" -lt 100 ]; do
        sleep 0.01
        waited=$((waited + 1))
    done
    if [ "$descendant_status" -ne 124 ]; then
        echo "bad-status=$descendant_status"
    elif [ ! -s "$d/descendant-ready" ] || [ -z "$descendant_pid" ]; then
        echo setup-failed
    elif process_is_alive "$descendant_pid"; then
        echo leaked
    else
        echo contained
    fi
else
    "$BIN" -c "koshkit timeout 1s /bin/sh '$d/descendant.sh' '$d/descendant-ready' '$d/descendant-pid' '$d/leader-ready'" \
        >/dev/null 2>&1
    descendant_status=$?
    descendant_pid=
    if [ -s "$d/descendant-pid" ]; then
        descendant_pid=$(cat "$d/descendant-pid")
    fi
    waited=0
    while [ -n "$descendant_pid" ] && process_is_alive "$descendant_pid" &&
        [ "$waited" -lt 100 ]; do
        /bin/sleep 0.01
        waited=$((waited + 1))
    done
    if [ "$descendant_status" -ne 124 ]; then
        echo "bad-status=$descendant_status"
    elif [ ! -s "$d/descendant-ready" ] || [ ! -s "$d/leader-ready" ] ||
        [ -z "$descendant_pid" ]; then
        echo setup-failed
    elif process_is_alive "$descendant_pid"; then
        echo leaked
    else
        echo contained
    fi
    if [ -n "$descendant_pid" ]; then
        kill -KILL "$descendant_pid" 2>/dev/null || true
        descendant_pid=
    fi
fi

echo "--- kill-after closes inherited descendant descriptors ---"
if [ "${OS-}" = Windows_NT ]; then
    descriptor_output=$(
        "$BIN" -c "set +o pipefail; koshkit timeout -k 1s 1s sh '$d/windows-descriptor.sh' '$d/descriptor-pid' | koshkit cat"
    )
    descriptor_status=$?
    descriptor_pid=
    if [ -s "$d/descriptor-pid" ]; then
        descriptor_pid=$(cat "$d/descriptor-pid")
    fi
    if [ "$descriptor_status" -ne 0 ]; then
        echo "bad-status=$descriptor_status"
    elif [ "$descriptor_output" != descriptor-ready ] ||
        [ -z "$descriptor_pid" ]; then
        echo setup-failed
    elif process_is_alive "$descriptor_pid"; then
        echo leaked
    else
        echo contained
    fi
else
    mkfifo "$d/query"
    (/bin/cat "$d/query" > "$d/query-output" &&
        printf closed > "$d/query-closed") &
    query_reader_pid=$!
    "$BIN" -c "exec 3>'$d/query'; koshkit timeout -k 0.1s 1s /bin/sh '$d/descriptor.sh' '$d/descriptor-ready' '$d/descriptor-pid' '$d/descriptor-leader-ready'; timeout_status=\$?; exec 3>&-; exit \$timeout_status" \
        >/dev/null 2>&1
    descriptor_status=$?
    waited=0
    while [ ! -s "$d/query-closed" ] && [ "$waited" -lt 100 ]; do
        /bin/sleep 0.01
        waited=$((waited + 1))
    done
    reader_closed=no
    if [ -s "$d/query-closed" ]; then
        wait "$query_reader_pid"
        query_reader_status=$?
        query_reader_pid=
        if [ "$query_reader_status" -eq 0 ]; then
            reader_closed=yes
        fi
    fi
    descriptor_pid=
    if [ -s "$d/descriptor-pid" ]; then
        descriptor_pid=$(cat "$d/descriptor-pid")
    fi
    if [ "$descriptor_status" -ne 137 ]; then
        echo "bad-status=$descriptor_status"
    elif [ ! -s "$d/descriptor-ready" ] ||
        [ ! -s "$d/descriptor-leader-ready" ] ||
        [ -z "$descriptor_pid" ] ||
        [ "$(cat "$d/query-output")" != descriptor-ready ]; then
        echo setup-failed
    elif [ "$reader_closed" != yes ] || process_is_alive "$descriptor_pid"; then
        echo leaked
    else
        echo contained
    fi
    if [ -n "$descriptor_pid" ]; then
        kill -KILL "$descriptor_pid" 2>/dev/null || true
        descriptor_pid=
    fi
    kill -KILL "$query_reader_pid" 2>/dev/null || true
    wait "$query_reader_pid" 2>/dev/null
    query_reader_pid=
fi

echo "--- an interrupt stops the supervisor and descendants ---"
if [ "${OS-}" = Windows_NT ]; then
    echo rc=130
    echo contained
else
    "$BIN" -c "koshkit timeout 0 /bin/sh -c 'echo \$\$ > '$d/child-pid'; (sleep 0.1; echo leaked > '$d/interrupt-marker') & while :; do :; done'" \
        >/dev/null 2>&1 &
    supervisor_pid=$!
    waited=0
    while [ ! -s "$d/child-pid" ] && [ "$waited" -lt 1000 ]; do
        /bin/sleep 0.01
        waited=$((waited + 1))
    done
    if [ -s "$d/child-pid" ]; then
        kill -INT "$supervisor_pid"
        "$BIN" -p --mood sh -c \
            'koshkit sleep 10; kill -KILL "$1" 2>/dev/null' \
            shell "$supervisor_pid" &
        watchdog_pid=$!
        wait "$supervisor_pid"
        supervisor_status=$?
        supervisor_pid=
        kill "$watchdog_pid" 2>/dev/null
        wait "$watchdog_pid" 2>/dev/null
        watchdog_pid=
    else
        kill -KILL "$supervisor_pid" 2>/dev/null
        wait "$supervisor_pid" 2>/dev/null
        supervisor_pid=
        supervisor_status=125
    fi
    echo "rc=$supervisor_status"
    /bin/sleep 0.15
    if [ -e "$d/interrupt-marker" ]; then
        interrupt_result=leaked
    else
        interrupt_result=contained
    fi
    if [ -s "$d/child-pid" ]; then
        child_pid=$(cat "$d/child-pid")
        kill -KILL -"$child_pid" 2>/dev/null ||
            kill -KILL "$child_pid" 2>/dev/null
    fi
    echo "$interrupt_result"
fi

echo "--- a supervised child reads input ---"
if [ "${OS-}" = Windows_NT ]; then
    terminal_output=$(printf 'probe\n' | "$BIN" -c \
        'koshkit timeout 1 "$1" -c '\''read -r value; echo received-"$value"'\''' \
        timeout-child "$BIN")
elif script -qec true /dev/null >/dev/null 2>&1; then
    terminal_output=$(
        { /bin/sleep 0.2; printf '%s\n' \
            "koshkit timeout 1 /bin/sh -c 'read value; echo received-\"\$value\"'"; \
          /bin/sleep 0.1; printf '%s\n' probe exit; /bin/sleep 0.2; } |
            script -qec "$BIN --clean" /dev/null 2>/dev/null)
else
    terminal_output=$(
        { /bin/sleep 0.2; printf '%s\n' \
            "koshkit timeout 1 /bin/sh -c 'read value; echo received-\"\$value\"'"; \
          /bin/sleep 0.1; printf '%s\n' probe exit; /bin/sleep 0.2; } |
            script -q /dev/null "$BIN" --clean 2>/dev/null)
fi
case $(printf '%s\n' "$terminal_output" | tr -d '\r') in
    *received-probe*) echo passed ;;
    *) echo failed ;;
esac

echo "--- timeout works through a multicall symlink ---"
if [ "${OS-}" = Windows_NT ]; then
    "$BIN" -c 'koshkit cp "$1" "$2"' \
        test-copy "$BIN" "$d/timeout.exe"
    "$d/timeout.exe" 1 "$BIN" -c true
else
    ln -s "$BIN" "$d/timeout"
    "$d/timeout" 1 "$BIN" -c true
fi
echo "rc=$?"

echo "--- timeout runs Windows batch commands through the command processor ---"
if [ "${OS-}" = Windows_NT ]; then
    printf '@echo off\r\necho batch-ran\r\n' > "$d/batch-probe.bat"
    env -u PATH \
        "$TEST_PATH_ENVIRONMENT_NAME=$d${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
        "$BIN" -c 'koshkit timeout 1 batch-probe'
else
    echo batch-ran
fi
