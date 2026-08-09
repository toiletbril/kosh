unset SHIT_FLAGS
# touch sets the modification time of an existing file to the current time rather
# than passing it over. A hermetic temp directory keeps it stable, and it is left
# in place so the test never runs rm.
# GNU stat reads the modification epoch with -c, BSD stat with -f, so the helper
# tries the GNU form first and falls back to the BSD one on macOS.
stat_mtime() { stat -c '%Y' "$1" 2>/dev/null || stat -f '%m' "$1"; }
dir=$(mktemp -d)
: > "$dir/f"
before=$(stat_mtime "$dir/f")
sleep 1.1
"$BIN" -c "shitbox touch '$dir/f'" </dev/null
after=$(stat_mtime "$dir/f")
echo "== touch advances the modification time of an existing file:"
if [ "$after" -gt "$before" ]; then
  echo advanced
else
  echo unchanged
fi
echo "== touch -c still does not create a missing file:"
"$BIN" -c "shitbox touch -c '$dir/ghost'" </dev/null
"$BIN" -c "if [ -e '$dir/ghost' ]; then echo ghost-exists; else echo ghost-missing; fi" </dev/null
