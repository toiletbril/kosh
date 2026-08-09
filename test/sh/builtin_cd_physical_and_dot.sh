#!/bin/sh
# cd -P resolves the symlinks to the physical directory under dash too, and the
# dot command ignores trailing operands the way dash does, so the caller's
# positional parameters carry through.
d=$(mktemp -d)
mkdir -p "$d/real"
ln -sfn "$d/real" "$d/link"
echo "physical=$(cd -P "$d/link" && pwd | grep -o '/real$')"
(set -P) 2>/dev/null
echo "set-physical=$?"
(set +P) 2>/dev/null
echo "unset-physical=$?"
(set -o physical) 2>/dev/null
echo "set-long-physical=$?"
file=$(mktemp)
printf 'echo "in=$1"\n' > "$file"
set -- keep
. "$file" ignored
echo "out=$1"
rm -rf "$d" "$file"


# CDPATH resolves a relative cd operand against a directory list, checked against
# dash. Only stable markers are printed, never the pid-specific temporary path.

base=/tmp/shit_cdpath_$$
rm -rf "$base"
mkdir -p "$base/sub" "$base/other"

# A nonempty entry resolves the operand, and the basename confirms the landing.
CDPATH=$base
cd sub >/dev/null 2>&1 && echo "landed=$(basename "$(pwd)")"

# A move through a nonempty entry announces the directory on stdout.
cd "$base" >/dev/null 2>&1
CDPATH=$base
out=$(cd other)
test -n "$out" && echo announce_nonempty

# An empty entry names the current directory and stays silent.
cd "$base" >/dev/null 2>&1
CDPATH="$TEST_PATH_SEPARATOR$base"
out=$(cd sub)
test -z "$out" && echo cwd_entry_silent

# A ./ operand skips CDPATH and resolves against the current directory.
cd "$base" >/dev/null 2>&1
CDPATH=$base
cd ./other >/dev/null 2>&1 && echo "dot_landed=$(basename "$(pwd)")"

cd / >/dev/null 2>&1
rm -rf "$base"
echo done

# Tilde expansion of a named user, compared byte-for-byte against dash. Both
# shells resolve the same system users, so the output is machine-independent for
# users present in the local database such as root.
echo ~root
echo ~root/sub/dir
echo ~nouser123
echo "~root"
x=~root
echo "$x"
echo a~root
echo ~root:~root
echo ~
