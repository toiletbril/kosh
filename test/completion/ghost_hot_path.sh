set -e

d=$(mktemp -d)
trap 'test -n "$d" && /bin/rm -rf "$d"' EXIT

result_field()
{
    printf '%s\n' "$1" | while IFS='=' read -r field value; do
        if [ "$field" = "$2" ]; then
            printf '%s\n' "$value"
            break
        fi
    done
}

assert_field()
{
    test "$(result_field "$1" "$2")" = "$3"
}

mkdir "$d/commands"
for name in probe-alpha probe-beta; do
    printf '#!/bin/sh\n' > "$d/commands/$name"
    chmod +x "$d/commands/$name"
done
unrelated_index=0
while [ "$unrelated_index" -lt 32 ]; do
    printf '#!/bin/sh\n' > "$d/commands/unrelated-$unrelated_index"
    chmod +x "$d/commands/unrelated-$unrelated_index"
    unrelated_index=$((unrelated_index + 1))
done

command_result=$(env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$d/commands" \
    "$BIN" --debug-ghost-at 'probe')
assert_field "$command_result" count 2
assert_field "$command_result" prefix probe-
assert_field "$command_result" materialized 0
test "$(result_field "$command_result" source-scans)" -le 128
echo 'command ghost skips unrelated PATH names'

missing_result=$(env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$d/commands" \
    "$BIN" --debug-ghost-at 'zzzz-missing')
assert_field "$missing_result" count 0
test "$(result_field "$missing_result" source-scans)" -le 128
echo 'command ghost misses stay bounded'

mkdir "$d/duplicate"
printf '#!/bin/sh\n' > "$d/duplicate/echo"
chmod +x "$d/duplicate/echo"
duplicate_result=$(env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$d/duplicate" \
    "$BIN" --debug-ghost-at 'ec')
assert_field "$duplicate_result" count 1
assert_field "$duplicate_result" prefix echo
assert_field "$duplicate_result" materialized 0
echo 'command ghost deduplicates sources'

mkdir "$d/filesystem"
cp "$d/commands/probe-alpha" "$d/commands/probe-beta" "$d/filesystem"
filesystem_index=0
while [ "$filesystem_index" -lt 32 ]; do
    : > "$d/filesystem/unrelated-$filesystem_index"
    filesystem_index=$((filesystem_index + 1))
done
filesystem_result=$(PATH=/bin "$BIN" \
    --debug-ghost-at "echo $d/filesystem/probe")
assert_field "$filesystem_result" count 2
assert_field "$filesystem_result" prefix "$d/filesystem/probe-alpha"
assert_field "$filesystem_result" source-scans 2
assert_field "$filesystem_result" materialized 0
echo 'filesystem ghost skips unrelated directory entries'

: > "$d/filesystem/foo_bar_baz"
fuzzy_result=$(PATH=/bin "$BIN" \
    --debug-ghost-at "echo $d/filesystem/fbb")
assert_field "$fuzzy_result" count 0
fuzzy_tab_result=$(PATH=/bin "$BIN" \
    --debug-complete-at "echo $d/filesystem/fbb")
printf '%s\n' "$fuzzy_tab_result" | grep -q "$d/filesystem/foo_bar_baz"
echo 'filesystem ghost leaves fuzzy matching to tab'

if [ "${OS-}" != Windows_NT ]; then
    mkdir "$d/identity"
    printf '#!/bin/sh\n' > "$d/identity/identity-probe"
    chmod +x "$d/identity/identity-probe"
    /bin/ln -s "$d/identity" "$d/identity-alias"
    identity_result=$(PATH="$d/identity-alias" "$BIN" \
        --debug-ghost-at "echo $d/identity/identity")
    assert_field "$identity_result" directory-stats 2
    assert_field "$identity_result" directory-reads 1
fi
echo 'directory aliases share one listing'
