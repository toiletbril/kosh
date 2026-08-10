#!/bin/sh

unset KOSH_FLAGS

echo '== alias:'
"$BIN" -c 'alias missing_name' 2>&1
echo "rc=$?"

echo '== hash:'
"$BIN" -c 'hash missing_name' 2>&1
echo "rc=$?"

echo '== command verbose:'
"$BIN" -c 'command -V missing_name' 2>&1
echo "rc=$?"

echo '== jobspec:'
"$BIN" -c 'jobs %999' 2>&1
echo "rc=$?"

echo '== timeout command:'
"$BIN" -c 'koshkit timeout 1 missing_name' 2>&1
echo "rc=$?"
