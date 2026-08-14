echo "== set --m:"
"$BIN" --debug-complete-at 'set --m' </dev/null
echo "== set --i:"
"$BIN" --debug-complete-at 'set --i' </dev/null
echo "== set --mood value:"
"$BIN" --debug-complete-at 'set --mood ' </dev/null
echo "== set --init-moods value:"
"$BIN" --debug-complete-at 'set --init-moods ' </dev/null
echo "== set -M value:"
"$BIN" --debug-complete-at 'set -M ' </dev/null
echo "== set -L value:"
"$BIN" --debug-complete-at 'set -L ' </dev/null
echo "== set --mood= value:"
"$BIN" --debug-complete-at 'set --mood=' </dev/null
echo "== set --init-moods= value:"
"$BIN" --debug-complete-at 'set --init-moods=' </dev/null
echo "== set -M= value:"
"$BIN" --debug-complete-at 'set -M=' </dev/null
echo "== set --mood= prefix:"
"$BIN" --debug-complete-at 'set --mood=b' </dev/null
echo "== kosh --mood value:"
"$BIN" --debug-complete-at 'kosh --mood ' </dev/null
echo "== kosh --mood= value:"
"$BIN" --debug-complete-at 'kosh --mood=' </dev/null
echo "== kosh -M value:"
"$BIN" --debug-complete-at 'kosh -M ' </dev/null
echo "== kosh --init-moods value:"
"$BIN" --debug-complete-at 'kosh --init-moods ' </dev/null
echo "== kosh -L= prefix:"
"$BIN" --debug-complete-at 'kosh -L=k' </dev/null
echo "== assimilate --link-mood value:"
"$BIN" --debug-complete-at 'assimilate --link-mood ' </dev/null
echo "== assimilate --link-mood= prefix:"
"$BIN" --debug-complete-at 'assimilate --link-mood=d' </dev/null
echo "== assignment prefix is untouched:"
"$BIN" --debug-complete-at 'MOOD=' </dev/null | grep -c '^bash$'
