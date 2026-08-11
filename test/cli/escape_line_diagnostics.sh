#!/bin/sh

unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")

d=$(mktemp -d) || exit 1
start=$PWD
trap 'cd "$start" && [ -n "$d" ] && rm -rf "$d"' EXIT

printf '#!/bin/sh\necho one \\ \necho two\n' > "$d/blank-continuation.sh"
printf '#!/bin/sh\necho one \\\t\necho two\n' > "$d/tab-continuation.sh"
printf '#!/bin/sh\necho one \\\necho two\n' > "$d/clean-continuation.sh"

echo '== blank after a continuation:'
"$BIN" -n -WWW "$d/blank-continuation.sh" 2>&1 | sed "s|$d|<tmp>|g"

echo '== tab after a continuation:'
"$BIN" -n -WWW "$d/tab-continuation.sh" 2>&1 | sed "s|$d|<tmp>|g"

echo '== a clean continuation stays quiet:'
"$BIN" -n -WWW "$d/clean-continuation.sh" 2>&1 | sed "s|$d|<tmp>|g"
echo "rc=$?"
