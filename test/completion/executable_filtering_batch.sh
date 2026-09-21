# Filesystem completion keeps executable and directory candidates while
# filtering non-executable and broken entries.
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT

printf '#!/bin/sh\n' > "$dir/executable"
: > "$dir/non-executable"
mkdir "$dir/directory"
chmod +x "$dir/executable"

if [ "${OS-}" = Windows_NT ]; then
    : > "$dir/symlink-executable.exe"
    symlink_executable=symlink-executable
else
    ln -s executable "$dir/symlink-executable"
    ln -s directory "$dir/symlink-directory"
    ln -s missing "$dir/broken-link"
    symlink_executable=symlink-executable
fi

cd "$dir" || exit 1
command_result=$($BIN --debug-complete-at './e' </dev/null)
case "$command_result" in
    *executable*) ;;
    *) exit 1 ;;
esac
case "$command_result" in
    *non-executable*) exit 1 ;;
    *) ;;
esac
printf 'command executable=%s\n' "$command_result"

directory_result=$($BIN --debug-complete-at './d' </dev/null)
case "$directory_result" in
    *directory*) ;;
    *) exit 1 ;;
esac
printf 'directory=%s\n' "$directory_result"

if [ "${OS-}" != Windows_NT ]; then
    symlink_result=$($BIN --debug-complete-at './symlink-' </dev/null)
    case "$symlink_result" in
        *symlink-executable*) ;;
        *) exit 1 ;;
    esac
    case "$symlink_result" in
        *symlink-directory*) ;;
        *) exit 1 ;;
    esac
    case "$symlink_result" in
        *broken-link*) exit 1 ;;
        *) ;;
    esac
    printf 'symlinks=%s\n' "$symlink_result"
fi

mkdir "$dir/first" "$dir/second"
printf '#!/bin/sh\n' > "$dir/second/duplicate"
chmod +x "$dir/second/duplicate"
duplicate_result=$(env PATH="$dir/first$TEST_PATH_SEPARATOR$dir/second$TEST_PATH_SEPARATOR$TEST_SYSTEM_PATH" \
    "$BIN" --debug-complete-at dupl </dev/null)
case "$duplicate_result" in
    *duplicate*) ;;
    *) exit 1 ;;
esac
printf 'duplicate-path=%s\n' "$duplicate_result"
