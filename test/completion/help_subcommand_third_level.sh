# An allowlisted command whose subcommand chain runs three deep. The top-level
# --help lists subcommands, the second level lists its own sub-subcommands, and
# the third level lists that sub-subcommand's options. Completion forks each
# level when the settled words name a parsed subcommand chain, capped at the
# depth limit. A fake binary named for an allowlisted command keeps the
# candidates stable across machines.
dir=$(mktemp -d) || exit 1
trap 'test -n "$dir" && rm -rf "$dir"' EXIT
mkdir -p "$dir"
chmod 755 "$dir"
cat > "$dir/docker" <<'SH'
#!/bin/sh
if [ "$1" = "compose" ] && [ "$2" = "config" ]; then
  echo "OPTIONS"
  echo "  --services   List the services"
  echo "  --volumes    List the volumes"
elif [ "$1" = "compose" ]; then
  echo "Commands:"
  echo "  up        Start the services"
  echo "  config    Show the resolved config"
else
  echo "Commands:"
  echo "  compose   Run a compose file"
  echo "  run       Run a command"
fi
SH
chmod +x "$dir/docker"
mkdir -p "$dir/man/man1"
cat > "$dir/man/man1/docker.1" <<'EOF'
.TH DOCKER 1
.SH SYNOPSIS
\fBdocker\fR [OPTIONS] COMMAND
EOF
cat > "$dir/man/man1/docker-compose.1" <<'EOF'
.TH DOCKER-COMPOSE 1
.SH SYNOPSIS
\fBdocker\fR \fBcompose\fR
EOF
echo "== first-level subcommands:"
MANPATH= PATH="$dir${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'docker ' </dev/null
echo "== help fallback after a nonmatching man subcommand:"
MANPATH="$dir/man" PATH="$dir${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'docker r' </dev/null
echo "== second-level sub-subcommands from 'docker compose --help':"
MANPATH= PATH="$dir${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'docker compose ' </dev/null
echo "== third-level options from 'docker compose config --help':"
MANPATH= PATH="$dir${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'docker compose config --' </dev/null
echo "== an unknown deeper word does not fork:"
MANPATH= PATH="$dir${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --debug-complete-at 'docker compose bogus --' </dev/null
