unset KOSH_FLAGS
# mkdir -m sets the named directory to the exact mode the way POSIX requires,
# so a bit the umask would clear is still set. A hermetic temp directory keeps
# it stable, and it is left in place so the test never runs rm.
# GNU stat reads the octal mode with -c, BSD stat with -f, so the helper tries
# the GNU form first and falls back to the BSD one on macOS.
stat_mode() { stat -c '%a' "$1" 2>/dev/null || stat -f '%Lp' "$1"; }
dir=$(mktemp -d)
echo "== -m 755 under umask 077 keeps every bit (want 755):"
(umask 077; "$BIN" -c "koshkit mkdir -m 755 '$dir/exact'") </dev/null
stat_mode "$dir/exact"
echo "== -p -m 777 names the deep directory exactly, parents stay umask-narrowed:"
(umask 022; "$BIN" -c "koshkit mkdir -p -m 777 '$dir/a/b/named'") </dev/null
printf 'named (want 777): '; stat_mode "$dir/a/b/named"
printf 'parent (want 755): '; stat_mode "$dir/a"
