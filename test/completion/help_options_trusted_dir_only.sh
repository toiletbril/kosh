# Option completion from a command's --help text passes two gates. The command
# is on kosh's allowlist of commands safe to fork, and it resolves into a
# directory the current user or root owns that is not writable by group or
# other. A fake binary named for an allowlisted command (act) drives the
# probe deterministically. The same binary under a name kosh does not recognize,
# or in a world-writable directory, is never forked.
workspace=$(mktemp -d) || exit 1
trap 'test -n "$workspace" && rm -rf "$workspace"' EXIT
trusted=$workspace/trusted
untrusted=$workspace/untrusted
marker=$workspace/marker
mkdir -p "$trusted" "$untrusted"
chmod 755 "$trusted"
chmod 777 "$untrusted"
export KOSH_HELP_MARKER=$marker
write_probe() {
  cat > "$1" <<'SH'
#!/bin/sh
echo forked >> "$KOSH_HELP_MARKER"
echo "  --marker-option   a probe option"
SH
  chmod +x "$1"
}
write_probe "$trusted/act"
write_probe "$trusted/helpprobe"
write_probe "$untrusted/act"

rm -f "$marker"
echo "== allowlisted command in a trusted directory offers its --help options:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'act --mark' </dev/null
echo "== and was forked:"
if [ -f "$marker" ]; then echo "forked"; else echo "not forked"; fi

rm -f "$marker"
echo "== a command not on the allowlist is never forked, even when trusted:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'helpprobe --mark' </dev/null
echo "== and was never forked:"
if [ -f "$marker" ]; then echo "forked"; else echo "not forked"; fi

rm -f "$marker"
echo "== an allowlisted command in a world-writable directory is never forked:"
PATH="$untrusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'act --mark' </dev/null
echo "== and was never forked:"
if [ -f "$marker" ]; then echo "forked"; else echo "not forked"; fi

cat > "$trusted/act" <<'SH'
#!/bin/sh
echo attempted >> "$KOSH_HELP_MARKER"
sleep 2
SH
chmod +x "$trusted/act"
rm -f "$marker"
echo "== a timed out help command is attempted once:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'act --mark' </dev/null
test "$(wc -l < "$marker")" -eq 1 && echo "attempted once"
