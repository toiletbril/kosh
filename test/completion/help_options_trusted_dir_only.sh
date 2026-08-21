workspace=$(mktemp -d) || exit 1
trap 'test -n "$workspace" && /bin/rm -rf "$workspace"' EXIT
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
if [ "$1" = "sync" ] && [ "$2" = "--help" ]; then
  echo "  --force   force synchronization"
  exit
fi
if [ "$1" != "--help" ]; then exit; fi
cat <<'HELP'
COMMANDS
  sync   synchronize state

OPTIONS
  --marker-option   a probe option
HELP
SH
  chmod +x "$1"
}
write_probe "$trusted/act"
write_probe "$trusted/helpprobe"
write_probe "$trusted/writableprobe"
write_probe "$untrusted/act"
write_probe "$untrusted/helpprobe"
chmod 777 "$trusted/writableprobe"
printf '%s\n' \
  '#!/bin/sh' \
  'echo attempted >> "$KOSH_HELP_MARKER"' \
  'dd if=/dev/zero bs=1048576 count=5 2>/dev/null' \
  'echo "  --late-option  must not be parsed"' > "$trusted/noisyprobe"
chmod +x "$trusted/noisyprobe"

rm -f "$marker"
echo "== allowlisted command in a trusted directory offers its --help options:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'act --mark' </dev/null
echo "== and was forked:"
if [ -f "$marker" ]; then echo "forked"; else echo "not forked"; fi

rm -f "$marker"
echo "== a generic command in a trusted directory offers its --help options:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'helpprobe --mark' </dev/null
echo "== and was forked:"
if [ -f "$marker" ]; then echo "forked"; else echo "not forked"; fi

echo "== a generic command offers parsed subcommands:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'helpprobe sy' </dev/null
echo "== a generic subcommand offers its own flags:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'helpprobe sync --for' </dev/null

rm -f "$marker"
echo "== a writable command in a trusted directory is never forked:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'writableprobe --mark' </dev/null
echo "== and was never forked:"
if [ -f "$marker" ]; then echo "forked"; else echo "not forked"; fi

rm -f "$marker"
echo "== oversized help output is rejected before trailing flags:"
PATH="$trusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'noisyprobe --late' </dev/null
test "$(wc -l < "$marker")" -eq 1 && echo "attempted once"

rm -f "$marker"
echo "== an allowlisted command in a world-writable directory is never forked:"
PATH="$untrusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'act --mark' </dev/null
echo "== and was never forked:"
if [ -f "$marker" ]; then echo "forked"; else echo "not forked"; fi

rm -f "$marker"
echo "== a generic command in a world-writable directory is never forked:"
PATH="$untrusted${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'helpprobe --mark' </dev/null
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
