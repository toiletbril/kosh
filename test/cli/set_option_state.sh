unset KOSH_FLAGS

directory=$(mktemp -d)
trap 'test -n "$directory" && /bin/rm -rf "$directory"' EXIT

echo "== standard option state:"
"$BIN" -M bash -c '
set -E -T -B -h -k
[[ -o errtrace ]] && echo errtrace-on
[[ -o functrace ]] && echo functrace-on
[[ -o braceexpand ]] && echo braceexpand-on
[[ -o hashall ]] && echo hashall-on
[[ -o keyword ]] && echo keyword-on
'

echo "== brace expansion follows its option:"
"$BIN" -M bash -c "set +B; printf '<%s>\n' {a,b}; set -B; printf '<%s>\n' {c,d}"

echo "== vi and emacs can both be off:"
"$BIN" -M bash -c '
if [[ ! -o vi ]] && [[ ! -o emacs ]]; then
    echo noninteractive-editing-off
fi
'
"$BIN" -M bash -c '
set -o vi
set +o vi
[[ -o vi ]] || echo vi-off
[[ -o emacs ]] || echo emacs-off
'

echo "== command substitution isolates option state:"
"$BIN" -M bash -c 'set +b; value=$(set -b); set -o | grep "^notify"'

echo "== runtime noexec stops later commands:"
noexec_output=$("$BIN" -M bash -c 'set -n; echo leaked')
if [ -z "$noexec_output" ]; then
    echo noexec-clean
else
    echo noexec-leaked
fi
if "$BIN" -M bash -c 'if set -n; then echo leaked; fi' \
    > "$directory/nested-noexec"
then
    if [ ! -s "$directory/nested-noexec" ]; then
        echo nested-noexec-clean
    else
        echo nested-noexec-leaked
    fi
fi
noexec_loop_output=$directory/noexec-loops
noexec_loop_status=0
for noexec_script in \
    'while set -n; do echo leaked; done' \
    'while true; do set -n; echo leaked; done' \
    'for ((;;)); do set -n; echo leaked; done'
do
    NOEXEC_BIN=$BIN NOEXEC_SCRIPT=$noexec_script "$BIN" -c \
        'koshkit timeout 1 "$NOEXEC_BIN" -M bash -c "$NOEXEC_SCRIPT"' \
        >> "$noexec_loop_output" 2>&1 || noexec_loop_status=1
done
if [ "$noexec_loop_status" -eq 0 ] && [ ! -s "$noexec_loop_output" ]; then
    echo noexec-loops-stop
else
    echo noexec-loops-failed
fi

echo "== function option changes survive defining-mood state:"
"$BIN" -M bash -c '
function enable_noclobber { set -C; }
set +C
set --mood kosh
enable_noclobber
case $- in *C*) echo function-option-persisted;; *) echo function-option-lost;; esac
'
"$BIN" -M kosh -c '
function enable_strict_options {
    set -u
    set -o pipefail
    set -o failglob
}
set --mood bash
enable_strict_options
if [[ -o nounset ]] && [[ -o pipefail ]] && [[ -o failglob ]]; then
    echo strict-markers-persisted
else
    echo strict-markers-lost
fi
'
"$BIN" -M bash -c '
set -o no-diagnostics
function apply_definition_state {
    set --mood bash
    set +W
    set -o no-diagnostics
}
set +o no-diagnostics
set -W
set --mood kosh
apply_definition_state
case $- in *W*) warning_level_clear=no;; *) warning_level_clear=yes;; esac
if [[ "$(set --mood)" = bash ]] &&
    [[ "$warning_level_clear" = yes ]] && [[ -o no-diagnostics ]]; then
    echo function-runtime-state-persisted
else
    echo function-runtime-state-lost
fi
'
"$BIN" -M bash -c '
function enable_posix_mode { set -o posix; }
set --mood kosh
enable_posix_mode
if [[ -o posix ]] && [[ "$(set --mood)" = bash-posix ]]; then
    echo function-posix-persisted
else
    echo function-posix-lost
fi
'
"$BIN" -M bash -c '
function disable_posix_noop { set +o posix; }
set --mood kosh
disable_posix_noop
if [[ "$(set --mood)" = kosh ]]; then
    echo function-posix-noop-stayed
else
    echo function-posix-noop-leaked
fi
'
"$BIN" -M bash -c '
function enable_no_diagnostics { set -o no-diagnostics; }
set +o annoying-diagnostics
enable_no_diagnostics
if [[ ! -o annoying-diagnostics ]] && [[ -o no-diagnostics ]]; then
    echo diagnostic-options-independent
else
    echo diagnostic-options-crossed
fi
'
"$BIN" -M bash -c '
function disable_annoying_diagnostics { set +o annoying-diagnostics; }
set -o annoying-diagnostics
disable_annoying_diagnostics
if [[ ! -o annoying-diagnostics ]]; then
    echo annoying-option-persisted
else
    echo annoying-option-lost
fi
'
"$BIN" -M bash -c '
function isolate_revision_state {
    ignored=$(set --mood sh
        set -W
        set -o no-diagnostics)
}
set --mood kosh
set +W
set +o no-diagnostics
isolate_revision_state
case $- in *W*) warning_level_clear=no;; *) warning_level_clear=yes;; esac
if [[ "$(set --mood)" = kosh ]] &&
    [[ "$warning_level_clear" = yes ]] && [[ ! -o no-diagnostics ]]; then
    echo substitution-revisions-isolated
else
    echo substitution-revisions-leaked
fi
'
"$BIN" -M bash -c '
set +o annoying-diagnostics
ignored=$(set -o annoying-diagnostics)
if [[ ! -o annoying-diagnostics ]]; then
    echo annoying-substitution-isolated
else
    echo annoying-substitution-leaked
fi
'

echo "== command strings publish c in dollar dash:"
"$BIN" -M bash -c 'case $- in *c*) echo command-string;; *) echo missing;; esac'

echo "== interactive state is independent of hashall:"
send_interactive_input()
{
    sleep 0.1
    printf '[[ -o emacs ]] && echo inter""active-emacs\nset +h\ncase $- in *i*) echo inter""active-letter;; *) :;; esac\nexit\n'
}
if script --version >/dev/null 2>&1; then
    send_interactive_input |
        BIN="$BIN" script -q -c 'exec "$BIN" -i --rcfile /dev/null' \
            "$directory/interactive" >/dev/null 2>&1
else
    send_interactive_input |
        BIN="$BIN" script -q "$directory/interactive" /bin/sh -c \
            'exec "$BIN" -i --rcfile /dev/null' >/dev/null 2>&1
fi
if strings "$directory/interactive" | grep -q 'interactive-letter' &&
    strings "$directory/interactive" | grep -q 'interactive-emacs'
then
    echo interactive
else
    echo missing
fi

echo "== command substitution isolates startup state:"
/bin/mkdir -p "$directory/home"
printf 'ignored=$(set --mood sh)\nignored=$(set --init-moods=sh)\n' \
    > "$directory/home/.koshrc"
send_snapshot_input()
{
    sleep 0.5
    printf 'printf "snapshot-""mood=%%s\\n" "$(set --mood)"\nprintf "snapshot-""moods=%%s\\n" "$(set --init-moods)"\nexit\n'
}
if script --version >/dev/null 2>&1; then
    send_snapshot_input |
        HOME="$directory/home" ENV=/dev/null BIN="$BIN" script -q -c \
            'exec "$BIN" -i -M bash --init-moods=kosh' \
            "$directory/startup-state" >/dev/null 2>&1
else
    send_snapshot_input |
        HOME="$directory/home" ENV=/dev/null BIN="$BIN" script -q \
            "$directory/startup-state" /bin/sh -c \
            'exec "$BIN" -i -M bash --init-moods=kosh' >/dev/null 2>&1
fi
if strings "$directory/startup-state" | grep -q 'snapshot-mood=bash' &&
    strings "$directory/startup-state" | grep -q 'snapshot-moods=kosh'
then
    echo startup-state-isolated
else
    echo startup-state-leaked
fi

echo "== command substitution preserves directory identity:"
/bin/mkdir -p "$directory/snapshot-parent/original" \
    "$directory/snapshot-parent/sibling"
: > "$directory/snapshot-parent/original/from-original"
SNAPSHOT_PARENT="$directory/snapshot-parent" "$BIN" -c '
    cd "$SNAPSHOT_PARENT/original" || exit
    ignored=$(mv "$SNAPSHOT_PARENT/original" "$SNAPSHOT_PARENT/renamed"
        cd "$SNAPSHOT_PARENT/sibling")
    [[ -f from-original ]] && [[ "$PWD" = "$SNAPSHOT_PARENT/original" ]] &&
        echo directory-identity-preserved
'

echo "== command substitution restores a permission-revoked directory:"
/bin/mkdir -p "$directory/snapshot-permissions/original" \
    "$directory/snapshot-permissions/sibling"
: > "$directory/snapshot-permissions/original/from-original"
SNAPSHOT_PARENT="$directory/snapshot-permissions" "$BIN" -c '
    cd "$SNAPSHOT_PARENT/original" || exit
    ignored=$(chmod 000 .
        cd "$SNAPSHOT_PARENT/sibling")
    second=$(printf ok)
    chmod 700 "$SNAPSHOT_PARENT/original"
    [[ "$second" = ok ]] && [[ -f from-original ]] &&
        echo permission-directory-preserved
'
chmod 700 "$directory/snapshot-permissions/original"

echo "== command substitution enters from an execute-only directory:"
/bin/mkdir -p "$directory/snapshot-execute-only"
chmod 111 "$directory/snapshot-execute-only"
SNAPSHOT_DIRECTORY="$directory/snapshot-execute-only" "$BIN" -c '
    cd "$SNAPSHOT_DIRECTORY" || exit
    value=$(printf ok)
    [[ "$value" = ok ]] && echo execute-only-directory-preserved
'
chmod 700 "$directory/snapshot-execute-only"

echo "== SHELLOPTS uses the shared option state:"
"$BIN" -M bash -c '
set -B -h
case :$SHELLOPTS: in *:braceexpand:*) echo braceexpand-listed;; esac
case :$SHELLOPTS: in *:hashall:*) echo hashall-listed;; esac
set -o export-all
set -o no-clobber
if [[ "$SHELLOPTS" = \
    allexport:braceexpand:hashall:interactive-comments:noclobber ]]; then
    echo shellopts-generated-order
else
    echo shellopts-order-broken
fi
'

echo "== physical mode resolves a symlinked directory:"
/bin/mkdir -p "$directory/real/child" "$directory/logical"
ln -s "$directory/real" "$directory/link"
expected=$(cd "$directory/real" && pwd -P)
actual=$("$BIN" -M bash -c "set -P; cd '$directory/link'; pwd")
if [ "$actual" = "$expected" ]; then
    echo physical=yes
else
    echo physical=no
fi
ln -s "$directory/real/child" "$directory/logical/link"
actual=$(CDPATH="$directory/logical/link/.." "$BIN" -M bash -c \
    'set -P; cd child >/dev/null; pwd')
expected_cdpath=$(cd "$directory/real/child" && pwd -P)
if [ "$actual" = "$expected_cdpath" ]; then
    echo physical-cdpath=yes
else
    echo physical-cdpath=no
fi
actual=$("$BIN" -M bash -c "set -P; cd '$directory/logical/link/..'; pwd")
if [ "$actual" = "$expected" ]; then
    echo physical-dotdot=yes
else
    echo physical-dotdot=no
fi

echo "== cd uses native path syntax:"
if [ "${OS-}" = Windows_NT ]; then
    path_separator='\'
    path_delimiter=';'
else
    path_separator='/'
    path_delimiter=':'
fi
/bin/mkdir -p "$directory/cdpath-root/child" \
    "$directory/cdpath-root/local" "$directory/local" "$directory/absolute"
: > "$directory/cdpath-root/child/cdpath-marker"
: > "$directory/cdpath-root/local/remote-marker"
: > "$directory/local/local-marker"
: > "$directory/absolute/absolute-marker"
CDPATH="$directory/missing$path_delimiter$directory/cdpath-root" \
    "$BIN" -c \
    'cd child >/dev/null && [[ -f cdpath-marker ]] && echo native-cdpath'
DOT_OPERAND=".${path_separator}local" START_DIRECTORY="$directory" \
    CDPATH="$directory/cdpath-root" "$BIN" -c \
    'cd "$START_DIRECTORY" || exit; cd "$DOT_OPERAND" || exit; [[ -f local-marker ]] && echo native-dot-relative'
ABSOLUTE_OPERAND="$directory/absolute" "$BIN" -c \
    'cd "$ABSOLUTE_OPERAND" || exit; [[ -f absolute-marker ]] && echo native-absolute'
if [ "${OS-}" = Windows_NT ]; then
    native_directory=$(START_DIRECTORY="$directory" "$BIN" -c \
        'cd "$START_DIRECTORY" && pwd -P')
    drive_prefix=${native_directory%%:*}:
    if ! DRIVE_ONLY="$drive_prefix" DRIVE_RELATIVE="${drive_prefix}local" \
        START_DIRECTORY="$directory" "$BIN" -c '
        cd "$START_DIRECTORY" || exit
        cd "$DRIVE_ONLY" || exit
        [[ -d local ]]
        cd "$DRIVE_RELATIVE" || exit
        [[ -f local-marker ]]
    '
    then
        exit 1
    fi
fi
