unset KOSH_FLAGS
BIN=$(cd "$(dirname "$BIN")" && pwd -P)/$(basename "$BIN")
start_directory=$(pwd -P)
root=$TEST_TEMP_DIRECTORY/lint-input-batch
mkdir -p "$root/directory"
root=$(cd "$root" && pwd -P)
trap 'cd "$start_directory" && [ -n "$root" ] && /bin/rm -rf "$root"' EXIT

cat > "$root/first.bash" <<'EOF'
#!/bin/bash
echo "$LINT_FILE_ONE"
printf run > lint-input-executed
EOF

cat > "$root/second.bash" <<'EOF'
#!/bin/bash
echo "$LINT_FILE_TWO"
EOF

cat > "$root/parse.bash" <<'EOF'
#!/bin/bash
if
EOF

cat > "$root/after-parse.bash" <<'EOF'
#!/bin/bash
echo "$LINT_AFTER_FILE_PARSE"
EOF

cat > "$root/-dash.bash" <<'EOF'
#!/bin/bash
echo "$LINT_DASH_FILE"
EOF

cat > "$root/stdin.bash" <<'EOF'
#!/bin/bash
echo "$LINT_STDIN"
EOF

diagnostic_names()
{
  names=
  while IFS= read -r name; do
    if [ -n "$names" ]; then
      names=$names,$name
    else
      names=$name
    fi
  done
  printf '%s' "$names"
}

cd "$root" || exit 1

output=$(
  "$BIN" --lint -WWW --no-traces \
    -c 'echo "$LINT_COMMAND_ONE"' \
    -c 'echo "$LINT_COMMAND_TWO"' \
    first.bash second.bash 2>&1
)
rc=$?
names=$(printf '%s\n' "$output" |
  sed -n \
    -e "s/.*The variable '\([^']*\)'.*/\1/p" \
    -e 's/^Encountered 4 errors\.$/SUMMARY/p' |
  diagnostic_names)
summary_count=$(printf '%s\n' "$output" |
  grep -c '^Encountered 4 errors\.$')
executed=0
[ -e lint-input-executed ] && executed=1
printf 'batch-order=%s summaries=%s rc=%s executed=%s\n' \
  "$names" "$summary_count" "$rc" "$executed"

output=$(
  "$BIN" --mood bash --lint --no-traces \
    -c 'echo "$LINT_COMMAND_ONE"' \
    -c 'echo "$LINT_COMMAND_TWO"' \
    first.bash second.bash 2>&1
)
rc=$?
names=$(printf '%s\n' "$output" |
  sed -n \
    -e "s/.*The variable '\([^']*\)'.*/\1/p" \
    -e 's/^Encountered 4 warnings\.$/SUMMARY/p' |
  diagnostic_names)
error_count=$(printf '%s\n' "$output" | grep -c ': error:')
summary_count=$(printf '%s\n' "$output" |
  grep -c '^Encountered 4 warnings\.$')
first_filename_count=$(printf '%s\n' "$output" | grep -c 'first.bash:2:')
second_filename_count=$(printf '%s\n' "$output" | grep -c 'second.bash:2:')
printf 'bash-order=%s errors=%s summaries=%s files=%s,%s rc=%s\n' \
  "$names" "$error_count" "$summary_count" "$first_filename_count" \
  "$second_filename_count" "$rc"

output=$("$BIN" --mood bash --lint --show-memory --no-traces \
  -c 'echo "$LINT_COMMAND_ONE"' -c 'echo "$LINT_COMMAND_TWO"' 2>&1)
last_line=$(printf '%s\n' "$output" | tail -n 1)
printf 'memory-summary-last=%s\n' \
  "$(test "$last_line" = 'Encountered 2 warnings.' && printf yes)"

output=$(
  "$BIN" --lint -t --no-traces \
    -c 'echo "$LINT_COMMAND_ONE"' \
    -c 'echo "$LINT_COMMAND_TWO"' \
    first.bash second.bash 2>&1
)
rc=$?
names=$(printf '%s\n' "$output" |
  sed -n "s/.*The variable '\([^']*\)'.*/\1/p" |
  diagnostic_names)
printf 'one-command-order=%s rc=%s\n' "$names" "$rc"

output=$(
  "$BIN" --lint --no-traces \
    -c 'if' \
    -c 'echo "$LINT_AFTER_COMMAND_PARSE"' \
    parse.bash after-parse.bash 2>&1
)
rc=$?
command_continued=$(printf '%s\n' "$output" |
  grep -c "The variable 'LINT_AFTER_COMMAND_PARSE'")
file_continued=$(printf '%s\n' "$output" |
  grep -c "The variable 'LINT_AFTER_FILE_PARSE'")
parse_error_count=$(printf '%s\n' "$output" | grep -c 'Unterminated if')
printf 'parse-command=%s parse-file=%s errors=%s rc=%s\n' \
  "$command_continued" "$file_continued" "$parse_error_count" "$rc"

output=$(
  "$BIN" --lint --no-traces first.bash missing.bash directory second.bash 2>&1
)
rc=$?
missing_count=$(printf '%s\n' "$output" |
  grep -c "Could not open 'missing.bash'")
directory_count=$(printf '%s\n' "$output" |
  grep -c 'because the file is a directory')
after_open_error=$(printf '%s\n' "$output" |
  grep -c "The variable 'LINT_FILE_TWO'")
printf 'missing=%s directory=%s continued=%s rc=%s\n' \
  "$missing_count" "$directory_count" "$after_open_error" "$rc"

output=$("$BIN" -n -WWW --no-traces first.bash parse.bash 2>&1)
rc=$?
first_count=$(printf '%s\n' "$output" | grep -c "The variable 'LINT_FILE_ONE'")
second_count=$(printf '%s\n' "$output" | grep -c 'Unterminated if')
printf 'noexec-first=%s noexec-second=%s rc=%s\n' \
  "$first_count" "$second_count" "$rc"

output=$(
  "$BIN" -n -WWW --no-traces \
    -c 'echo "$NOEXEC_COMMAND_ONE"' \
    -c 'echo "$NOEXEC_COMMAND_TWO"' \
    parse.bash second.bash 2>&1
)
rc=$?
command_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'NOEXEC_COMMAND_")
file_count=$(printf '%s\n' "$output" | grep -c 'Unterminated if')
printf 'noexec-commands=%s noexec-files=%s rc=%s\n' \
  "$command_count" "$file_count" "$rc"

output=$("$BIN" --lint --no-traces -- -dash.bash second.bash 2>&1)
rc=$?
dash_count=$(printf '%s\n' "$output" | grep -c "The variable 'LINT_DASH_FILE'")
second_count=$(printf '%s\n' "$output" | grep -c "The variable 'LINT_FILE_TWO'")
printf 'dash-file=%s second-file=%s rc=%s\n' \
  "$dash_count" "$second_count" "$rc"

output=$("$BIN" --lint --no-traces < stdin.bash 2>&1)
rc=$?
stdin_count=$(printf '%s\n' "$output" | grep -c "The variable 'LINT_STDIN'")
printf 'stdin=%s rc=%s\n' "$stdin_count" "$rc"

output=$(
  "$BIN" --lint -s -i --no-traces \
    -c 'echo "$LINT_IGNORED_COMMAND"' parse.bash < stdin.bash 2>&1
)
rc=$?
stdin_count=$(printf '%s\n' "$output" | grep -c "The variable 'LINT_STDIN'")
command_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'LINT_IGNORED_COMMAND'")
file_count=$(printf '%s\n' "$output" | grep -c 'Unterminated if')
printf 'stdin-precedence=%s command=%s file=%s rc=%s\n' \
  "$stdin_count" "$command_count" "$file_count" "$rc"
