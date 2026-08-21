workspace=$(mktemp -d) || exit 1
trap 'test -n "$workspace" && /bin/rm -rf "$workspace"' EXIT
bin=$workspace/bin
manroot=$workspace/man
mandir=$manroot/man1
mkdir -p "$bin" "$mandir"
chmod 755 "$bin"

printf '%s\n' \
  '#!/bin/sh' \
  'if [ "$1" = "--help" ]; then' \
  '  printf "%s\n" "COMMANDS" "  status  Show status"' \
  'elif [ "$1" = "status" ] && [ "$2" = "--help" ]; then' \
  '  printf "%s\n" "  --force-status  Force status refresh"' \
  'fi' > "$bin/manprobe"
printf '%s\n' \
  '#!/bin/sh' \
  'case "$1" in' \
  '  --path) printf "%s\n" "$MANROOT" ;;' \
  '  -w) printf "%s/man1/%s.1\n" "$MANROOT" "$2" ;;' \
  '  manprobe)' \
  '    printf "%s\n" "COMMANDS" "  sync  Synchronize state" "  status  Show status" "" "OPTIONS" "  --alpha  Root option" "  --force-root  Root force option"' \
  '    ;;' \
  '  manprobe-sync)' \
  '    printf "%s\n" "OPTIONS" "  --force  Force synchronization" "  --format  Select output format"' \
  '    ;;' \
  'esac' > "$bin/man"
chmod +x "$bin/man" "$bin/manprobe"

printf '%s\n' '.TH MANPROBE 1' '.SH SYNOPSIS' 'manprobe' > "$mandir/manprobe.1"
printf '%s\n' '.TH MANPROBE-SYNC 1' '.SH SYNOPSIS' 'manprobe sync' > "$mandir/manprobe-sync.1"

echo "== root flags from the command manpage:"
MANPATH="$manroot" MANROOT="$manroot" PATH="$bin${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'manprobe --a' </dev/null
echo "== subcommands from the command manpage:"
MANPATH="$manroot" MANROOT="$manroot" PATH="$bin${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'manprobe sy' </dev/null
echo "== flags from the subcommand manpage:"
MANPATH="$manroot" MANROOT="$manroot" PATH="$bin${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'manprobe sync --f' </dev/null
echo "== subcommand flag prefix filtering:"
MANPATH="$manroot" MANROOT="$manroot" PATH="$bin${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'manprobe sync --forc' </dev/null
echo "== a subcommand without a manpage falls through to its own help:"
MANPATH="$manroot" MANROOT="$manroot" PATH="$bin${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'manprobe status --force' </dev/null
