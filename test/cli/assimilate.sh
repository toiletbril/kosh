#!/bin/sh

# shellcheck disable=SC2119,SC2120
root=$(mktemp -d)
trap '[ -n "$root" ] && /bin/rm -rf "$root"' EXIT
transport=$root/transport
remote=$root/remote
install=$remote/bin
binary=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
real_uname=$(command -v uname)
/bin/mkdir -p "$transport" "$install"

failure_runner=$root/failure-runner
cat > "$failure_runner" <<'SH'
#!/bin/sh
case $0 in
    *.upload|*.candidate) exec "$ASSIMILATE_RUNNER" "$@" ;;
    */kosh)
        install_directory=${0%/*}
        for lock in "$install_directory"/.kosh-assimilate-*.lock; do :; done
        /bin/ln -s . "$lock/committed"
        exec "$ASSIMILATE_RUNNER" "$@"
        ;;
    *) exec "$ASSIMILATE_RUNNER" "$@" ;;
esac
SH
chmod +x "$failure_runner"

cat > "$transport/scp" <<'SH'
#!/bin/sh
printf 'scp-run\n' >> "$ASSIMILATE_TRANSPORT_LOG"
if [ "${1-}" = scp-prefix ]; then
    printf 'scp-prefix\n' >> "$ASSIMILATE_TRANSPORT_LOG"
    shift
fi
[ "$1" = -p ] || exit 40
shift
[ "$1" = -- ] || exit 41
shift
[ "$#" -eq 2 ] || exit 42
destination=$2
source=$1
[ "${ASSIMILATE_FAILURE-}" != rollback ] || source=$ASSIMILATE_FAILURE_RUNNER
[ "${ASSIMILATE_FAIL_SCP-0}" -eq 0 ] || exit 23
case $destination in
    *']:'*) remote_name=${destination#*]:} ;;
    *) remote_name=${destination#*:} ;;
esac
/bin/cp -p "$source" "$ASSIMILATE_REMOTE/$remote_name"
[ "${ASSIMILATE_FAILURE-}" != identity ] || printf x >> "$ASSIMILATE_REMOTE/$remote_name"
SH
chmod +x "$transport/scp"

cat > "$transport/uname" <<'SH'
#!/bin/sh
case ${1-} in
    -s)
        if [ -n "${ASSIMILATE_REMOTE_SYSTEM-}" ]; then
            printf '%s\n' "$ASSIMILATE_REMOTE_SYSTEM"
            exit
        fi
        ;;
    -m)
        if [ -n "${ASSIMILATE_REMOTE_MACHINE-}" ]; then
            printf '%s\n' "$ASSIMILATE_REMOTE_MACHINE"
            exit
        fi
        ;;
esac
exec "$ASSIMILATE_REAL_UNAME" "$@"
SH
chmod +x "$transport/uname"

cat > "$transport/ssh" <<'SH'
#!/bin/sh
printf 'ssh-run\n' >> "$ASSIMILATE_TRANSPORT_LOG"
if [ "${1-}" = ssh-prefix ]; then
    printf 'ssh-prefix\n' >> "$ASSIMILATE_TRANSPORT_LOG"
    shift
fi
[ "$1" = -- ] || exit 40
shift
[ "$1" = 'user@[2001:db8::1]' ] || exit 41
shift
[ "$#" -eq 1 ] || exit 42
command=$1
case $command in
    "set -- "*"expected_system="*)
        PATH=$ASSIMILATE_TRANSPORT /bin/sh -c "$command"
        exit
        ;;
    *'/bin/'*) exit 43 ;;
    "exec ./.kosh-assimilate-"*.upload*) ;;
    "exec './.kosh-assimilate-"*.upload*) ;;
    *) exit 44 ;;
esac
cd "$ASSIMILATE_REMOTE" || exit 1
umask 077
HOME=$ASSIMILATE_REMOTE_HOME PATH=$ASSIMILATE_REMOTE_PATH /bin/sh -c "$command"
SH
chmod +x "$transport/ssh"

leftovers()
{
    find "$remote" -name '.kosh-assimilate-*' | wc -l | tr -d ' '
}

file_mode()
{
    mode=$(stat -c %a "$1" 2>/dev/null)
    case "$mode" in
        ''|*[!0-9]*) mode=$(stat -f %Lp "$1" 2>/dev/null) ;;
    esac
    printf '%s\n' "$mode"
}

run_assimilate()
{
    PATH="$transport${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
        "$BIN" -c 'assimilate "$@"' kosh \
        "$@" 'user@[2001:db8::1]' \
        >/dev/null 2>&1
}

export ASSIMILATE_REMOTE=$remote
export ASSIMILATE_REMOTE_PATH="$remote/missing:$install"
export ASSIMILATE_REMOTE_HOME=$remote/home
export ASSIMILATE_TRANSPORT_LOG=$root/transport.log
export ASSIMILATE_TRANSPORT=$transport
export ASSIMILATE_REAL_UNAME=$real_uname
export ASSIMILATE_FAILURE_RUNNER=$failure_runner
export ASSIMILATE_RUNNER=$binary
run_assimilate
assimilate_status=$?
if [ "$assimilate_status" -eq 0 ] && cmp "$BIN" "$install/kosh" >/dev/null &&
    [ -x "$install/kosh" ]; then
    success_status=0
else
    success_status=1
fi
printf 'success=%s leftovers=%s\n' "$success_status" "$(leftovers)"

printf 'architecture-old\n' > "$install/kosh"
: > "$ASSIMILATE_TRANSPORT_LOG"
export ASSIMILATE_REMOTE_SYSTEM="MismatchOS;:>$ASSIMILATE_REMOTE/injected"
export ASSIMILATE_REMOTE_MACHINE=mismatch-arch
architecture_output=$root/architecture.out
PATH="$transport${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
    "$BIN" -c 'assimilate --trace "$@"' kosh \
    'user@[2001:db8::1]' >"$architecture_output" 2>&1
architecture_status=$?
if [ "$architecture_status" -ne 0 ] &&
    [ "$(cat "$install/kosh")" = architecture-old ] &&
    grep -q '^+ assimilate: preflight$' "$architecture_output" &&
    grep -Fq "on $ASSIMILATE_REMOTE_SYSTEM/mismatch-arch" \
        "$architecture_output" &&
    [ ! -e "$ASSIMILATE_REMOTE/injected" ] &&
    ! grep -q '^scp-run$' "$ASSIMILATE_TRANSPORT_LOG"; then
    architecture_result=0
else
    architecture_result=1
fi
printf 'architecture-status=%s result=%s leftovers=%s\n' \
    "$architecture_status" "$architecture_result" "$(leftovers)"
unset ASSIMILATE_REMOTE_SYSTEM ASSIMILATE_REMOTE_MACHINE

stale_id=999999-1
stale_lock=$install/.kosh-assimilate-$stale_id.lock
physical_install=$(cd "$install" && pwd -P)
/bin/mkdir "$install/.kosh-assimilate-888888-1.lock"
/bin/mkdir "$stale_lock"
printf '999999:%s\n' "${stale_lock##*/}" > "$stale_lock/owner"
printf '.kosh-assimilate-%s.upload\n' "$stale_id" > "$stale_lock/upload"
printf '%s/.kosh-assimilate-%s.candidate\n' "$physical_install" "$stale_id" \
    > "$stale_lock/candidate"
printf 'identity-old\n' > "$stale_lock/backup"
printf 'identity-rejected\n' > "$install/kosh"
: > "$stale_lock/had-target"
: > "$stale_lock/rollback"
: > "$remote/.kosh-assimilate-$stale_id.upload"
: > "$install/.kosh-assimilate-$stale_id.candidate"
export ASSIMILATE_FAILURE=identity
run_assimilate
identity_status=$?
printf 'identity-status=%s old=%s leftovers=%s\n' "$identity_status" \
    "$(cat "$install/kosh")" "$(leftovers)"
unset ASSIMILATE_FAILURE

printf 'regular-old\n' > "$install/kosh"
chmod 751 "$install/kosh"
export ASSIMILATE_FAILURE=rollback
run_assimilate
rollback_status=$?
printf 'rollback-status=%s old=%s mode=%s leftovers=%s\n' "$rollback_status" \
    "$(cat "$install/kosh")" "$(file_mode "$install/kosh")" "$(leftovers)"
unset ASSIMILATE_FAILURE

printf 'transfer-old\n' > "$install/kosh"
export ASSIMILATE_FAIL_SCP=1
run_assimilate
transfer_status=$?
printf 'transfer-status=%s old=%s leftovers=%s\n' "$transfer_status" \
    "$(cat "$install/kosh")" "$(leftovers)"
unset ASSIMILATE_FAIL_SCP

/bin/mkdir -p "$ASSIMILATE_REMOTE_HOME/.local/bin" "$remote/sbin"
printf 'local-collision\n' > "$ASSIMILATE_REMOTE_HOME/.local/bin/bash"
export ASSIMILATE_REMOTE_PATH="$remote/sbin:$install:$ASSIMILATE_REMOTE_HOME/.local/bin"
: > "$ASSIMILATE_TRANSPORT_LOG"
trace_output=$root/trace.out
PATH="$transport${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
    "$BIN" -c 'assimilate "$@"' kosh \
    -x --ssh-command 'ssh ssh-prefix' --scp-command 'scp scp-prefix' \
    --link-mood bash,dash --link-mood sh 'user@[2001:db8::1]' \
    >"$trace_output" 2>&1
option_status=$?
if [ -L "$install/bash" ] && [ "$install/bash" -ef "$install/kosh" ] &&
    [ -L "$install/dash" ] && [ "$install/dash" -ef "$install/kosh" ] &&
    [ -L "$install/sh" ] && [ "$install/sh" -ef "$install/kosh" ]; then
    link_status=0
else
    link_status=1
fi
if grep -q '^scp-prefix$' "$ASSIMILATE_TRANSPORT_LOG" &&
    grep -q '^ssh-prefix$' "$ASSIMILATE_TRANSPORT_LOG"; then
    command_status=0
else
    command_status=1
fi
if grep -q '^+ assimilate: preflight$' "$trace_output" &&
    grep -q '^+ assimilate: upload$' "$trace_output" &&
    grep -q '^+ assimilate: install$' "$trace_output" &&
    grep -q '^+ umask 077$' "$trace_output"; then
    trace_status=0
else
    trace_status=1
fi
if [ ! -e "$remote/sbin/kosh" ]; then
    sbin_status=0
else
    sbin_status=1
fi
if [ "$(cat "$ASSIMILATE_REMOTE_HOME/.local/bin/bash")" = local-collision ]; then
    collision_status=0
else
    collision_status=1
fi
printf 'options=%s links=%s commands=%s trace=%s sbin=%s collision=%s leftovers=%s\n' \
    "$option_status" "$link_status" "$command_status" "$trace_status" \
    "$sbin_status" "$collision_status" "$(leftovers)"
