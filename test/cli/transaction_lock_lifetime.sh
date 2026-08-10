directory=$(mktemp -d) || exit 1
wrapper_process=
transaction_state=not_started
flock_command=$(command -v flock 2>/dev/null || :)
if [ -n "$flock_command" ] && \
    ! "$flock_command" --help 2>&1 | grep -q conflict-exit-code; then
    flock_command=
fi
busybox_command=
if [ -z "$flock_command" ]; then
    busybox_command=$(command -v busybox 2>/dev/null || :)
fi

wait_for_path()
{
    path=$1
    attempt_count=0
    while [ ! -e "$path" ] && [ "$attempt_count" -lt 1000 ]; do
        sleep 0.01
        attempt_count=$((attempt_count + 1))
    done
    test -e "$path"
}

probe_directory_lock()
{
    if [ "${OS-}" = Windows_NT ]; then
        lock_file=$directory/.kosh-flock.lock
        [ -e "$lock_file" ] || return 2
        powershell.exe -NoProfile -NonInteractive -Command '& {
            param($lock_path)
            try {
                $stream = [IO.File]::Open($lock_path, "OpenOrCreate", "ReadWrite", "None")
                $stream.Dispose()
                exit 0
            } catch [IO.IOException] {
                exit 1
            } catch {
                exit 2
            }
        }' "$lock_file" >/dev/null 2>&1
        return $?
    fi

    if [ -n "$flock_command" ]; then
        "$flock_command" -E 75 -n "$directory" true
        probe_status=$?
        case $probe_status in
        0) return 0 ;;
        75) return 1 ;;
        *) return 2 ;;
        esac
    fi

    if [ -n "$busybox_command" ]; then
        "$busybox_command" flock -n "$directory" true >/dev/null 2>&1
        probe_status=$?
        case $probe_status in
        0) return 0 ;;
        1) return 1 ;;
        *) return 2 ;;
        esac
    fi

    command -v perl >/dev/null 2>&1 || return 2
    perl -MFcntl=:flock -MErrno=EAGAIN,EWOULDBLOCK -e '
        open my $lock, "<", $ARGV[0] or exit 2;
        exit 0 if flock($lock, LOCK_EX | LOCK_NB);
        exit(($! == EAGAIN || $! == EWOULDBLOCK) ? 1 : 2);
    ' "$directory"
}

wait_for_lock_release()
{
    maximum_attempt_count=${1-1000}
    attempt_count=0
    while [ "$attempt_count" -lt "$maximum_attempt_count" ]; do
        probe_directory_lock
        probe_status=$?
        case $probe_status in
        0) return 0 ;;
        1) sleep 0.01 ;;
        *) return 1 ;;
        esac
        attempt_count=$((attempt_count + 1))
    done
    return 1
}

cleanup()
{
    if [ -z "$directory" ] || [ ! -d "$directory" ]; then
        return
    fi

    : > "$directory/release"
    if [ -n "$wrapper_process" ]; then
        kill -9 "$wrapper_process" 2>/dev/null || :
        wait "$wrapper_process" 2>/dev/null || :
    fi
    if [ "$transaction_state" = launched ]; then
        wait_for_path "$directory/started" 2>/dev/null || :
    fi
    if [ -e "$directory/started" ]; then
        wait_for_lock_release 2>/dev/null || :
    fi
    /bin/rm -rf "$directory"
}

trap cleanup EXIT

SHELL_BINARY=$BIN "$BIN" -p --mood sh -c '
    koshkit flock "$1" "$SHELL_BINARY" -p --mood sh -c \
        "printf held > \"\$1\"" shell "$2"
' shell "$directory" "$directory/normal"
printf 'normal=%s list=%s\n' "$(cat "$directory/normal")" \
    "$("$BIN" -c 'koshkit --list' | grep -c '^flock$')"
if [ "${OS-}" = Windows_NT ]; then
    wait_for_lock_release 10 || exit 1
else
    probe_directory_lock || exit 1
fi

LOCK_DIRECTORY=$directory STATE_DIRECTORY=$directory SHELL_BINARY=$BIN \
    "$BIN" -p --mood bash -c '
    koshkit flock --transaction-held-lock "$LOCK_DIRECTORY" "$SHELL_BINARY" \
        -p --mood sh -c '\''koshkit touch "$STATE_DIRECTORY/started"
        attempt_count=0
        while [ ! -e "$STATE_DIRECTORY/release" ] && [ "$attempt_count" -lt 3000 ]; do
            koshkit sleep 0.01
            attempt_count=$((attempt_count + 1))
        done
        test -e "$STATE_DIRECTORY/release" || exit 1
        koshkit touch "$STATE_DIRECTORY/finished"'\''
' &
wrapper_process=$!
transaction_state=launched
wait_for_path "$directory/started" || exit 1
transaction_state=started
kill -9 "$wrapper_process"
wait "$wrapper_process" 2>/dev/null || :
wrapper_process=

probe_directory_lock
probe_status=$?
if [ "$probe_status" -eq 1 ]; then
    echo 'transaction child retains the process lock'
else
    exit 1
fi

: > "$directory/release"
wait_for_path "$directory/finished" || exit 1
wait_for_lock_release || exit 1
LOCK_DIRECTORY=$directory STATE_DIRECTORY=$directory SHELL_BINARY=$BIN \
    "$BIN" -p --mood sh -c '
    printf "%s\n" "$$" > "$STATE_DIRECTORY/evaluator-pid"
    koshkit flock --transaction-held-lock "$LOCK_DIRECTORY" \
        "$SHELL_BINARY" -p --mood sh -c \
        '\''koshkit touch "$STATE_DIRECTORY/transaction-acquired"'\''
    koshkit flock "$LOCK_DIRECTORY" "$SHELL_BINARY" -p --mood sh -c \
        '\''set --mood bash
        printf "%s\n" "$PPID" > "$STATE_DIRECTORY/normal-parent-pid"
        koshkit touch "$STATE_DIRECTORY/acquired"'\''
'
test -e "$directory/transaction-acquired" || exit 1
test -e "$directory/acquired" || exit 1
test "$(cat "$directory/evaluator-pid")" = \
    "$(cat "$directory/normal-parent-pid")" || exit 1
transaction_state=complete
echo 'the next transaction acquires the released lock'
