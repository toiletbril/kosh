unset SHIT_FLAGS

echo "== bench points at the failing command, not the whole bench line:"
"$BIN" -c 'bench --runs 1 ls --color' 2>&1 | grep -A1 'error:'
echo "== kill points at the bad pid, not the whole kill line:"
"$BIN" -c 'kill notapid' 2>&1 | grep -A1 'error:'
echo "== break points at the bad count, not the whole break line:"
"$BIN" -c 'for i in 1 2; do break 0; done' 2>&1 | grep -A1 'error:'
echo "== exit points at the bad status, not the whole exit line:"
"$BIN" --no-diagnostics -c 'exit notanumber' 2>&1 | grep -A1 'Builtin `exit`'
echo "== umask points at the bad mask, not the whole umask line:"
"$BIN" -c 'umask 0999' 2>&1 | grep -A1 'error:'
echo "== set points at the bad option, not the whole set line:"
"$BIN" -c 'set -Z' 2>&1 | grep -A1 'error:'
echo "== z points at the bad query, not the whole z line:"
"$BIN" -c 'z no_such_dir_xyz' 2>&1 | grep -A1 'error:'
