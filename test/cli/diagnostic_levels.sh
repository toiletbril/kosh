temporary_directory=$(mktemp -d)
trap 'test -n "$temporary_directory" && /bin/rm -rf "$temporary_directory"' EXIT
script_command=$(command -v script)

echo '== command flag ordering:'
"$BIN" -c -W 'echo level-one-after-command' \
  -c -WW 'echo level-two-after-command' \
  -c -WWW 'echo level-three-after-command'

echo '== set diagnostic levels:'
"$BIN" -M bash -c '
case $- in *W*) :;; *) echo default-level;; esac
set -W
case $- in *WW*) :;; *W*) echo short-level-one;; esac
set -W
case $- in *WWW*) :;; *WW*) echo short-level-two;; esac
set -W
case $- in *WWW*) echo short-level-three;; esac
[[ -o annoying-diagnostics ]] && echo annoying-diagnostics-on
set +o annoying-diagnostics
[[ -o annoying-diagnostics ]] || echo annoying-diagnostics-off
set -o annoying-diagnostics
[[ -o annoying-diagnostics ]] && echo annoying-diagnostics-restored
set --mood kosh
case $- in *W*) :;; *) echo mood-reset-level;; esac
set -WWW
case $- in *WWW*) echo bundled-level-three;; esac
set +W
case $- in *W*) :;; *) echo disabled-short-level;; esac
'
for removed_name in warnings force-warnings force-diagnostics force-annoying-diagnostics; do
  "$BIN" -M bash -c "set -o $removed_name" >/dev/null 2>&1
  echo "legacy-name-$removed_name=$?"
done

"$BIN" -W -c 'echo [abc' >/dev/null 2>&1
echo "level-one-strict=$?"
"$BIN" -WW -c 'echo [abc' >/dev/null 2>&1
echo "level-two-strict=$?"
"$BIN" -WWW -c 'echo [abc' >/dev/null 2>&1
echo "level-three-strict=$?"

annoying_source='f(){ diagnostic_level_leak=1; cd missing; echo wrong-directory; }; echo survived'
"$BIN" -c "$annoying_source" >/dev/null 2>&1
echo "annoying-default=$?"
"$BIN" -W -c "$annoying_source" >/dev/null 2>&1
echo "annoying-level-one=$?"
"$BIN" --no-annoying-diagnostics -c "$annoying_source" >/dev/null 2>&1
echo "annoying-suppressed=$?"
runtime_annoying_output=$("$BIN" -W \
  -c 'set +o annoying-diagnostics' -c "$annoying_source" 2>&1)
echo "annoying-runtime-suppressed=$(printf '%s\n' "$runtime_annoying_output" |
  grep -c 'This .cd. is unchecked')"
runtime_annoying_output=$("$BIN" -W \
  -c 'set +o annoying-diagnostics; set -o annoying-diagnostics' \
  -c "$annoying_source" 2>&1)
echo "annoying-runtime-restored=$(printf '%s\n' "$runtime_annoying_output" |
  grep -c 'This .cd. is unchecked')"

lenient_source='diagnostic_levels_missing_command; echo survived'
"$BIN" -W -c "$lenient_source" >/dev/null 2>&1
echo "lenient-level-one=$?"
"$BIN" -WW -c "$lenient_source" >/dev/null 2>&1
echo "lenient-level-two=$?"

compat_source='f(){ cd missing; echo $[1+2]; }; wrapper(){ wrapper "$@"; }; echo survived'
compat_lenient_source='echo "$LATER_VALUE"; LATER_VALUE=set; echo survived'
compat_lenient_level_one=$("$BIN" -M bash -W -c "$compat_lenient_source" 2>&1)
compat_lenient_level_two=$("$BIN" -M bash -WW -c "$compat_lenient_source" 2>&1)
compat_level_two=$("$BIN" -M bash -WW -c "$compat_source" 2>&1)
compat_level_three=$("$BIN" -M bash -WWW -c "$compat_source" 2>&1)
echo "compat-level-one-lenient=$(printf '%s\n' "$compat_lenient_level_one" |
  grep -c 'read before it is assigned')"
echo "compat-level-two-lenient=$(printf '%s\n' "$compat_lenient_level_two" |
  grep -c 'read before it is assigned')"
echo "compat-level-two-annoying=$(printf '%s\n' "$compat_level_two" |
  grep -c 'This .cd. is unchecked')"
echo "compat-level-three-annoying=$(printf '%s\n' "$compat_level_three" |
  grep -c 'This .cd. is unchecked')"
echo "compat-level-two-obsolete=$(printf '%s\n' "$compat_level_two" |
  grep -c 'SC2007')"
echo "compat-level-three-obsolete=$(printf '%s\n' "$compat_level_three" |
  grep -c 'SC2007')"
echo "compat-level-two-recursion=$(printf '%s\n' "$compat_level_two" |
  grep -c 'SC2264')"
echo "compat-level-three-recursion=$(printf '%s\n' "$compat_level_three" |
  grep -c 'SC2264')"

cat > "$temporary_directory/file-directive.sh" <<'EOF'
# shellcheck disable=SC2164
f(){ cd missing; echo wrong-directory; }
echo file-directive
EOF
file_directive_output=$(
  "$BIN" "$temporary_directory/file-directive.sh" 2>&1
)
echo "$file_directive_output"
echo "file-directive-errors=$(printf '%s\n' "$file_directive_output" |
  grep -c 'This .cd. is unchecked')"

cat > "$temporary_directory/local-directive.sh" <<'EOF'
f() {
  # shellcheck disable='SC2080-SC2090'
  echo $suppressed
  # shellcheck disable=SC9999 disable=2086
  echo $repeated
  # shellcheck disable="all"
  echo $all
  echo $reported
  true && # shellcheck disable=SC2086
    echo $after_and
}
echo local-directive
EOF
local_directive_output=$(
  "$BIN" "$temporary_directory/local-directive.sh" 2>&1
)
echo "local-directive-warnings=$(printf '%s\n' "$local_directive_output" |
  grep -c 'An unquoted variable can split')"

cat > "$temporary_directory/numeric-variant-directive.sh" <<'EOF'
# shellcheck disable=SC2086
f() { echo $general; [ $tested = value ]; }
echo numeric-variant-directive
EOF
numeric_variant_output=$(
  "$BIN" "$temporary_directory/numeric-variant-directive.sh" 2>&1
)
echo "$numeric_variant_output"
echo "numeric-variant-errors=$(printf '%s\n' "$numeric_variant_output" |
  grep -c 'SC2086')"

cat > "$temporary_directory/slug-variant-directive.sh" <<'EOF'
# shellcheck disable=unquoted-expansion
f() { echo $general; [ $tested = value ]; }
echo slug-variant-directive
EOF
slug_variant_output=$(
  "$BIN" "$temporary_directory/slug-variant-directive.sh" 2>&1
)
echo "slug-variant-general=$(printf '%s\n' "$slug_variant_output" |
  grep -c 'split into words')"
echo "slug-variant-test=$(printf '%s\n' "$slug_variant_output" |
  grep -c 'unquoted test operand')"

cat > "$temporary_directory/native-slug-directive.sh" <<'EOF'
# shellcheck disable=no-local
f() { native_slug_value=1; }
echo native-slug-directive
EOF
native_slug_output=$(
  "$BIN" "$temporary_directory/native-slug-directive.sh" 2>&1
)
echo "$native_slug_output"
echo "native-slug-warnings=$(printf '%s\n' "$native_slug_output" |
  grep -c 'has no .local.')"

conditional_pattern_output=$(
  "$BIN" -n -c '[[ $arg != *=* ]]; [[ $arg == *=* ]]; [[ $arg == *"="* ]]' 2>&1
)
echo "conditional-pattern-errors=$(printf '%s\n' "$conditional_pattern_output" |
  grep -c 'without surrounding spaces')"

local_probe_output=$(
  "$BIN" -n -c 'local probe 2>/dev/null && probe_ran=1' 2>&1
)
echo "conditional-local-errors=$(printf '%s\n' "$local_probe_output" |
  grep -c '.local. outside a function')"

uncertain_slash_output=$(
  "$BIN" -WW -c "builtin eval 'function dynamic/name { :; }'; dynamic/name" 2>&1
)
echo "uncertain-slash-full-name=$(printf '%s\n' "$uncertain_slash_output" |
  grep -c "The command 'dynamic/name' could not be verified after runtime code")"
echo "uncertain-slash-prefix=$(printf '%s\n' "$uncertain_slash_output" |
  grep -c "The command 'dynamic' could not be verified after runtime code")"

send_runtime_input()
{
  sleep 0.1
  printf '%s\n' 'set -WW'
  sleep 0.1
  printf '%s\n' 'echo "[$LATER_DIAGNOSTIC]"; LATER_DIAGNOSTIC=x'
  sleep 0.1
  printf '%s\n' 'set -WWW'
  sleep 0.1
  printf '%s\n' 'case value in same) :;; same) :;; esac'
  sleep 0.1
  printf '%s\n' 'cd diagnostic-levels-missing'
  sleep 0.1
  printf '%s\n' "eval 'diagnostic_levels_missing_command'"
  sleep 0.1
  printf '%s\n' 'exit'
}
if "$script_command" --version >/dev/null 2>&1; then
  send_runtime_input | TERM=xterm-256color NO_COLOR= \
    KOSH_HISTORY_FILE="$temporary_directory/history" BIN="$BIN" \
    "$script_command" -q -c \
    '/bin/stty cols 100 rows 24; exec "$BIN" -M bash -i --rcfile /dev/null' \
    "$temporary_directory/typescript" >/dev/null 2>&1
else
  send_runtime_input | TERM=xterm-256color NO_COLOR= \
    KOSH_HISTORY_FILE="$temporary_directory/history" BIN="$BIN" \
    "$script_command" -q "$temporary_directory/typescript" /bin/sh -c \
    '/bin/stty cols 100 rows 24; exec "$BIN" -M bash -i --rcfile /dev/null' \
    >/dev/null 2>&1
fi
runtime_warning_count=$(strings "$temporary_directory/typescript" |
  grep -c "is read before it is assigned")
echo "runtime-level-two-warnings=$runtime_warning_count"
