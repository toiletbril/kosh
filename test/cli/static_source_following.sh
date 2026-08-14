unset KOSH_FLAGS
BIN=$(cd "$(dirname "$BIN")" && pwd -P)/$(basename "$BIN")
start_directory=$(pwd -P)
root=$TEST_TEMP_DIRECTORY/static-source-following
mkdir -p "$root/nested" "$root/path-bin" "$root/~" "$root/home"
root=$(cd "$root" && pwd -P)
if native_root=$(cd "$root" && pwd -W 2>/dev/null); then
  :
else
  native_root=$root
fi
trap 'cd "$start_directory" && [ -n "$root" ] && /bin/rm -rf "$root"' EXIT

cat > "$root/nested/root.bash" <<'EOF'
#!/bin/bash
source -- child.bash
. -- ./child.bash
dynamic_source=missing-dynamic.bash
source "$dynamic_source"
source missing-literal.bash
printf executed > should-not-exist
EOF
cat > "$root/child.bash" <<'EOF'
echo "$CHILD_SOURCE_UNSET"
source grandchild.bash
EOF
cat > "$root/nested/child.bash" <<'EOF'
echo decoy
EOF
cat > "$root/grandchild.bash" <<'EOF'
echo "$GRANDCHILD_SOURCE_UNSET"
. nested/root.bash
EOF
cat > "$root/path-bin/path-helper" <<'EOF'
echo "$PATH_SOURCE_UNSET"
EOF
cat > "$root/path-root.bash" <<'EOF'
source path-helper
EOF
cat > "$root/parse-root.bash" <<'EOF'
source parse-child.bash
EOF
cat > "$root/parse-child.bash" <<'EOF'
if
EOF
cat > "$root/state-root.bash" <<'EOF'
STATE_VALUE=ready
state_function() { :; }
(sourced_function() { :; }; source state-child.bash; sourced_function)
source state-child.bash
sourced_function
inner
EOF
cat > "$root/state-child.bash" <<'EOF'
echo "$STATE_VALUE"
state_function
sourced_function() { :; }
outer() { PATH=/tmp; inner() { :; }; }
EOF
cat > "$root/wrapper-root.bash" <<'EOF'
builtin source wrapper-source.bash
command . wrapper-dot.bash
EOF
cat > "$root/wrapper-source.bash" <<'EOF'
echo "$WRAPPER_SOURCE_UNSET"
EOF

cat > "$root/wrapper-dot.bash" <<'EOF'
echo "$WRAPPER_DOT_UNSET"
EOF
cat > "$root/tilde-root.bash" <<'EOF'
source "~/literal-child.bash"
source ~/active-child.bash
HOME=missing
source ~/stale-child.bash
EOF

cat > "$root/~/literal-child.bash" <<'EOF'
echo "$LITERAL_TILDE_UNSET"
EOF

cat > "$root/home/active-child.bash" <<'EOF'
echo "$ACTIVE_TILDE_UNSET"
EOF
cat > "$root/home/stale-child.bash" <<'EOF'
echo "$STALE_TILDE_UNSET"
EOF
cat > "$root/shebang-root.bash" <<'EOF'
#!/bin/bash
source shebang-child.bash
EOF

cat > "$root/shebang-child.bash" <<'EOF'
#!/bin/sh
values=(one two)
EOF

cat > "$root/path-mutation-root.bash" <<'EOF'
if false; then
  source path-mutation-child.bash
fi
source path-helper
EOF

cat > "$root/path-mutation-child.bash" <<'EOF'
PATH=path-bin
EOF
run_and_capture()
{
  output=$("$@" 2>&1)
  rc=$?
}

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
run_and_capture "$BIN" --mood bash --lint nested/root.bash
lint_order=$(printf '%s\n' "$output" | sed -n \
  -e "/The variable 'GRANDCHILD_SOURCE_UNSET'/s/.*/grandchild/p" \
  -e "/The variable 'CHILD_SOURCE_UNSET'/s/.*/child/p" \
  -e '/^Encountered 2 warnings\.$/s/.*/summary/p' | diagnostic_names)
summary_count=$(printf '%s\n' "$output" |
  grep -c '^Encountered 2 warnings\.$')
grandchild_filename_count=$(printf '%s\n' "$output" |
  grep -c 'grandchild.bash:1:')
child_filename_count=$(printf '%s\n' "$output" |
  grep -c "child.bash:1:.*'CHILD_SOURCE_UNSET'")
printf 'lint-order=%s summaries=%s files=%s,%s rc=%s\n' \
  "$lint_order" "$summary_count" "$grandchild_filename_count" \
  "$child_filename_count" "$rc"
test ! -e should-not-exist || exit 1
run_and_capture "$BIN" -n nested/root.bash
grandchild_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'GRANDCHILD_SOURCE_UNSET'")
child_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'CHILD_SOURCE_UNSET'")
printf 'noexec-grandchild=%s child=%s rc=%s\n' \
  "$grandchild_count" "$child_count" "$rc"
test ! -e should-not-exist || exit 1
run_and_capture "$BIN" --mood bash -n parse-root.bash
parse_count=$(printf '%s\n' "$output" | grep -c 'Unterminated if')
printf 'noexec-bash-parse=%s rc=%s\n' "$parse_count" "$rc"
run_and_capture "$BIN" --lint state-root.bash
inner_count=$(printf '%s\n' "$output" | grep -c "Command 'inner' was not found")
printf 'state-inner=%s rc=%s\n' "$inner_count" "$rc"
run_and_capture "$BIN" --lint wrapper-root.bash
source_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'WRAPPER_SOURCE_UNSET'")
dot_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'WRAPPER_DOT_UNSET'")
printf 'wrapper-source=%s dot=%s rc=%s\n' "$source_count" "$dot_count" "$rc"
run_and_capture env HOME="$root/home" "$BIN" --lint tilde-root.bash
literal_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'LITERAL_TILDE_UNSET'")
active_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'ACTIVE_TILDE_UNSET'")
stale_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'STALE_TILDE_UNSET'")
quoted_count=$(printf '%s\n' "$output" | grep -c '(SC2088)')
printf 'tilde-literal=%s active=%s stale=%s quoted=%s rc=%s\n' \
  "$literal_count" "$active_count" "$stale_count" "$quoted_count" "$rc"
run_and_capture "$BIN" --lint shebang-root.bash
shebang_output_count=$(printf '%s\n' "$output" | grep -c .)
printf 'sourced-shebang-output=%s rc=%s\n' "$shebang_output_count" "$rc"
run_and_capture env -u PATH \
  "$TEST_PATH_ENVIRONMENT_NAME=$native_root/path-bin" \
  "$BIN" --lint path-mutation-root.bash
path_followed_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'PATH_SOURCE_UNSET'")
path_assignment_count=$(printf '%s\n' "$output" | grep -c '(SC2123)')
printf 'path-mutation-followed=%s assignment=%s rc=%s\n' \
  "$path_followed_count" "$path_assignment_count" "$rc"
run_and_capture "$BIN" nested/root.bash
grandchild_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'GRANDCHILD_SOURCE_UNSET'")
child_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'CHILD_SOURCE_UNSET'")
printf 'ordinary-grandchild=%s child=%s rc=%s\n' \
  "$grandchild_count" "$child_count" "$rc"
test ! -e should-not-exist || exit 1
run_and_capture env -u PATH \
  "$TEST_PATH_ENVIRONMENT_NAME=$native_root/path-bin" \
  "$BIN" --lint path-root.bash
path_count=$(printf '%s\n' "$output" |
  grep -c "The variable 'PATH_SOURCE_UNSET'")
printf 'path-source=%s rc=%s\n' "$path_count" "$rc"
run_and_capture "$BIN" --lint parse-root.bash
parse_count=$(printf '%s\n' "$output" | grep -c 'Unterminated if')
printf 'parse-source=%s rc=%s\n' "$parse_count" "$rc"
