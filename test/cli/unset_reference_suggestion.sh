#!/bin/sh

unset KOSH_FLAGS

echo '== an unset name near a set name names it:'
"$BIN" -WWW -c 'output_file=out; echo "$output_fil"' 2>&1

echo '== a transposed pair names the set name:'
"$BIN" -WWW -c 'destination_path=/tmp; echo "$destinaton_path"' 2>&1

echo '== a name far from every set name keeps the plain note:'
"$BIN" -WWW -c 'destination_path=/tmp; echo "$zzzzzzzzzzzzzz"' 2>&1

echo '== nounset names the set name in its fatal note:'
"$BIN" -c 'set -u; configuration_root=/etc; echo "$configuration_rot"' 2>&1
echo "rc=$?"

echo '== an exported name is a candidate:'
KOSH_TEST_EXPORTED=1 "$BIN" -WWW -c 'echo "$KOSH_TEST_EXPORTE"' 2>&1
