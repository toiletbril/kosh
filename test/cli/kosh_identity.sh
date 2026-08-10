#!/bin/sh

directory=$(mktemp -d)
trap '[ -n "$directory" ] && rm -rf "$directory"' EXIT HUP INT TERM
export IDENTITY_TEST_DIRECTORY=$directory
if [ "${OS-}" = Windows_NT ]; then
    "$BIN" -c 'koshkit cp "$1" "$2"' \
        test-copy "$BIN" "$directory/entry.exe"
    identity_entry=$directory/entry.exe
    relative_identity_entry=./entry.exe
else
    ln -s "$BIN" "$directory/target"
    ln -s target "$directory/middle"
    ln -s middle "$directory/entry"
    identity_entry=$directory/entry
    relative_identity_entry=./entry
fi

validate_identity()
{
    "$@" -c '
        cd "$IDENTITY_TEST_DIRECTORY" || exit 1
        case $KOSH_IDENTITY in
            *[!0-9a-f]*|"") exit 1 ;;
            *) [ "${#KOSH_IDENTITY}" -eq 8 ] ;;
        esac
    '
}

validate_identity "$identity_entry" || exit 1
(cd "$directory" && validate_identity "$relative_identity_entry") || exit 1
if [ "${OS-}" != Windows_NT ]; then
    (PATH="$directory${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH"
     validate_identity entry) || exit 1
fi

KOSH_IDENTITY=forged "$BIN" -c '
    identity=$KOSH_IDENTITY
    case $identity in
        *[!0-9a-f]*|"") valid=0 ;;
        *)
            if [ "${#identity}" -eq 8 ]; then
                valid=1
            else
                valid=0
            fi
            ;;
    esac
    koshkit env > "$IDENTITY_TEST_DIRECTORY/environment-output"
    exported_line=$(koshkit grep "KOSH_IDENTITY=$identity" \
        "$IDENTITY_TEST_DIRECTORY/environment-output")
    if [ "$exported_line" = "KOSH_IDENTITY=$identity" ]; then
        exported=1
    else
        exported=0
    fi
    readonly -p > "$IDENTITY_TEST_DIRECTORY/readonly-output"
    readonly_line=$(koshkit grep "readonly KOSH_IDENTITY=" \
        "$IDENTITY_TEST_DIRECTORY/readonly-output")
    case $readonly_line in
        "readonly KOSH_IDENTITY="*) readonly_status=1 ;;
        *) readonly_status=0 ;;
    esac
    printf "valid=%s exported=%s readonly=%s\n" \
        "$valid" "$exported" "$readonly_status"
'

if [ "${OS-}" = Windows_NT ]; then
    materialized_environment=$(KOSH_IDENTITY=forged "$BIN" -c \
        'koshkit timeout 1 cmd.exe /d /c set 2>"${TEST_NULL_DEVICE:-/dev/null}"')
else
    materialized_environment=$(KOSH_IDENTITY=forged "$BIN" -c \
        'koshkit timeout 1 /usr/bin/env')
fi
printf '%s\n' "$materialized_environment" |
    grep '^KOSH_IDENTITY=' |
    sed 's/=.*/=materialized/'
