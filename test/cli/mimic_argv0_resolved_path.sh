unset KOSH_FLAGS
# A mimicked script found on PATH reads $0 as the resolved script path, the
# argument the kernel hands a shebang interpreter, so realpath "$0" finds the
# script's true directory the way envman derives its env dir.
bin=$(mktemp -d)
cat > "$bin/argv0probe" <<'EOF'
#!/bin/bash
echo "zero=$0"
test "$0" = "$BASH_SOURCE" && echo match
EOF
chmod +x "$bin/argv0probe"
output=$(env -u PATH \
    "$TEST_PATH_ENVIRONMENT_NAME=$bin${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
    "$BIN" --enable-mimicry -c 'argv0probe')
status=$?
printf '%s\n' "$output" | sed 's|\\|/|g' | sed "s|$bin|BINDIR|"
rm -rf "$bin"
echo "rc=$status"
