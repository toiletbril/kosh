#!/bin/sh

unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")

d=$(mktemp -d) || exit 1
start=$PWD
trap 'cd "$start" && [ -n "$d" ] && rm -rf "$d"' EXIT
mkdir "$d/source" "$d/project" "$d/project/nested" "$d/actual-directory"
mkdir "$d/existing" "$d/sibling"
mkdir "$d/expanded-parent"
mkdir "$d/space dir"
mkdir "$d/target" "$d/target/nested"
mkdir "$d/cdpath" "$d/cdpath/target" "$d/cdpath/target/nested"
mkdir "$d/cdpath/sibling"
ln -s actual-directory "$d/linked-directory"
ln -s project "$d/logical"
ln -s target/nested "$d/link"
ln -s target/nested "$d/cdpath/link"
printf 'data\n' > "$d/regular-file"
printf '#!/bin/sh\necho should-not-run\n' > "$d/other"
chmod +x "$d/other"
printf '#!/bin/sh\necho symlink-target\n' > "$d/target/other"
chmod +x "$d/target/other"

cd "$d" || exit 1

echo '== transposition:'
"$BIN" -c 'cd soruce' 2>&1 | sed 's|\\|/|g'

echo '== nested path:'
"$BIN" -c 'cd project/nseted' 2>&1 | sed 's|\\|/|g'

echo '== directory symlink:'
"$BIN" -c 'cd linked-directroy' 2>&1 | sed 's|\\|/|g'

echo '== quoted suggestion:'
"$BIN" -c 'cd "space dri"' 2>&1 | sed 's|\\|/|g'

echo '== regular file is excluded:'
"$BIN" -c 'cd regular-fiel' 2>&1 | sed 's|\\|/|g'

echo '== distant miss:'
"$BIN" -c 'cd nowhere-near' 2>&1 | sed 's|\\|/|g'

echo '== first missing expanded component:'
BASE="$d" "$BIN" -c 'cd "$BASE/expanded-parent/miss/tail"' 2>&1 |
    sed -e 's|\\|/|g' -e "s|$d|<tmp>|g"

echo '== intermediate regular file:'
"$BIN" -c 'cd regular-file/child' 2>&1 | sed 's|\\|/|g'

echo '== direct regular file:'
"$BIN" -c 'cd regular-file' 2>&1 | sed 's|\\|/|g'

echo '== option terminator and missing tail:'
"$BIN" -c 'cd -- project/nseted/tail' 2>&1 | sed 's|\\|/|g'

echo '== one-byte missing component:'
"$BIN" -c 'cd x/tail' 2>&1 | sed 's|\\|/|g'

echo '== dot traversal:'
"$BIN" -c 'cd project/../miss/tail' 2>&1 | sed 's|\\|/|g'

echo '== logical symlink traversal:'
"$BIN" -c 'cd -L logical/nseted/tail' 2>&1 | sed 's|\\|/|g'

echo '== analyzed command path first missing component:'
"$BIN" -c './project/miss/tail' 2>&1 | sed 's|\\|/|g'

echo '== runtime command path first missing component:'
"$BIN" --no-diagnostics -c './project/miss/tail' 2>&1 | sed 's|\\|/|g'

echo '== exec command path first missing component:'
"$BIN" --no-diagnostics -c 'exec ./project/miss/tail' 2>&1 | sed 's|\\|/|g'

echo '== analyzed command path preserves missing dot-dot component:'
"$BIN" -c './missing/../other' 2>&1 | sed 's|\\|/|g'

echo '== runtime command path preserves missing dot-dot component:'
"$BIN" --no-diagnostics -c './missing/../other' 2>&1 | sed 's|\\|/|g'

echo '== exec command path preserves missing dot-dot component:'
"$BIN" --no-diagnostics -c 'exec ./missing/../other' 2>&1 | sed 's|\\|/|g'

echo '== cd preserves missing dot-dot component:'
"$BIN" -c 'cd missing/../existing' 2>&1 | sed 's|\\|/|g'

echo '== logical cd preserves symlink traversal:'
"$BIN" -c 'cd -L link/../sibling && pwd' 2>&1 |
    sed -e 's|\\|/|g' -e "s|$d|<tmp>|g"

echo '== analyzed trailing separator preserves missing dot-dot component:'
"$BIN" -c './missing/../existing/' 2>&1 | sed 's|\\|/|g'

echo '== runtime trailing separator preserves missing dot-dot component:'
"$BIN" --no-diagnostics -c './missing/../existing/' 2>&1 | sed 's|\\|/|g'

echo '== exec trailing separator preserves missing dot-dot component:'
"$BIN" --no-diagnostics -c 'exec ./missing/../existing/' 2>&1 |
    sed 's|\\|/|g'

echo '== command path preserves symlink traversal:'
"$BIN" -c './link/../other' 2>&1 | sed 's|\\|/|g'

echo '== CDPATH logical traversal preserves symlink traversal:'
CDPATH="$d/cdpath" "$BIN" -c 'cd link/../sibling >/dev/null && pwd' 2>&1 |
    sed -e 's|\\|/|g' -e "s|$d|<tmp>|g"

if [ "${OS-}" = Windows_NT ]; then
    native_pwd=$("$BIN" -c pwd | sed 's|\\|/|g')
    drive_relative_operand="${native_pwd%%/*}missing/../other"
else
    drive_relative_operand='./missing/../other'
fi
out=$("$BIN" --no-diagnostics -c "$drive_relative_operand" 2>&1)
printf 'raw traversal missing=%s caret=%s executed=%s\n' \
    "$(printf '%s\n' "$out" | grep -Fc "The directory '${drive_relative_operand%%/../*}'")" \
    "$(printf '%s\n' "$out" | grep -c '^       |    \^~~~~~~$')" \
    "$(printf '%s\n' "$out" | grep -c 'should-not-run')"

echo '== CDPATH physical traversal preserves symlink semantics:'
CDPATH="$d/cdpath" "$BIN" -c 'cd -P link/../sibling && pwd' 2>&1 |
    sed -e 's|\\|/|g' -e "s|$d|<tmp>|g"

if [ "${OS-}" = Windows_NT ]; then
    path_separator='\'
else
    path_separator='/'
fi
native_operand="project${path_separator}nseted${path_separator}tail"
echo '== native separator:'
"$BIN" -c "cd '$native_operand'" 2>&1 |
    sed 's|\\|/|g; s|`'"'"'project/nested'"'"'`|`project/nested`|'
