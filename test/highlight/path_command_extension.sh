set -e

dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
if [ "${OS-}" = Windows_NT ]; then
    executable_name=explicit-probe.exe
else
    executable_name=EXPLICIT-PROBE.EXE
fi
printf '#!/bin/sh\n' > "$dir/$executable_name"
chmod +x "$dir/$executable_name"
printf '#!/bin/sh\n' > "$dir/collision.com"
chmod +x "$dir/collision.com"
mkdir "$dir/directory.exe"
if [ "${OS-}" = Windows_NT ]; then
    mkdir "$dir/foo"
else
    : > "$dir/foo"
fi
printf '#!/bin/sh\n' > "$dir/foobar"
chmod +x "$dir/foobar"

tab=$(printf '\t')
env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$dir" \
    "$BIN" --debug-highlight-at \
    'EXPLICIT-PROBE.EXE; collision.exe; directory.exe' |
    grep -E "${tab}(resolved-command|partial-command|unknown-command)$"
env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$dir" \
    "$BIN" --debug-highlight-at 'EXPLICIT-P' |
    grep -E "${tab}partial-command$"
env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$dir" \
    "$BIN" --debug-highlight-at 'explicit-p' |
    grep -E "${tab}partial-command$"
env -u PATH "$TEST_PATH_ENVIRONMENT_NAME=$dir" \
    "$BIN" --debug-highlight-at 'foo' |
    grep -E "${tab}partial-command$"

mkdir "$dir/blocker-first" "$dir/blocker-second"
mkdir "$dir/blocker-first/blocked.exe"
printf '#!/bin/sh\n' > "$dir/blocker-second/blocked.exe"
chmod +x "$dir/blocker-second/blocked.exe"
env -u PATH \
    "$TEST_PATH_ENVIRONMENT_NAME=$dir/blocker-first${TEST_PATH_SEPARATOR}$dir/blocker-second" \
    "$BIN" --debug-highlight-at 'blocked.exe' |
    grep -E "${tab}resolved-command$"

PATH=/nonexistent "$BIN" --debug-highlight-at 'npr' |
    grep -E "${tab}partial-command$"
PATH=/nonexistent "$BIN" --debug-highlight-at 'ec\ho' |
    grep -E "${tab}resolved-command$"
mkdir "$dir/existing"
(
    cd "$dir"
    PATH=/nonexistent "$BIN" --debug-highlight-at 'ech>./existing/out'
) | grep -E "${tab}(unknown-command|operator|existing-path|invalid-path)$"

"$BIN" --debug-highlight-at './kos' |
    grep -E "${tab}(existing-path|partial-path)$"
"$BIN" --debug-highlight-at '~' | grep -Fx "~${tab}partial-path"
