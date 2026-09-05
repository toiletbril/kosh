"$BIN" --no-init-files -c '
value="a'"'"'b"
indexed=(zero one)
indexed[9]=nine
declare -A associated=([key]="map value")
state_function() { printf function; }
alias state_alias="printf alias"
set -- first second
set -o pipefail
expected_pwd=$PWD
pushd . >/dev/null
expected_dirs=$(dirs -p | koshkit wc -l)

printf "process="
koshkit cat < <(printf "%s:%s:%s:%s:%s:%s:%s:%s:%s" "$value" "${indexed[1]}" "${indexed[9]}" "${associated[key]}" "$(state_function)" "$*" "$0" "$([[ -o pipefail ]] && printf on)" "$([ "$PWD" = "$expected_pwd" ] && [ "$(dirs -p | koshkit wc -l)" = "$expected_dirs" ] && printf dirs)"; state_alias)
printf "\n"

printf "" | { printf "compound=%s:%s:%s:%s:%s:%s:%s:%s:%s\n" "$value" "${indexed[1]}" "${indexed[9]}" "${associated[key]}" "$(state_function)" "$*" "$0" "$([[ -o pipefail ]] && printf on)" "$([ "$PWD" = "$expected_pwd" ] && [ "$(dirs -p | koshkit wc -l)" = "$expected_dirs" ] && printf dirs)"; }
' child-name

"$BIN" --no-init-files --no-diagnostics -c '
false
koshkit cat < <(printf "status-process=%s\n" "$?")
false
printf "" | { printf "status-compound=%s\n" "$?"; }
'

"$BIN" --mood posix --no-init-files -c '
printf "mood="
koshkit cat < <(set --mood)
'

"$BIN" --restricted --no-init-files --no-diagnostics -c '
printf "restricted-process="
koshkit cat < <(
  if [[ $- == *r* ]]; then
    printf yes
  else
    printf no
  fi
)
printf "\n"
printf "" | {
  if [[ $- == *r* ]]; then
    printf "restricted-compound=yes\n"
  else
    printf "restricted-compound=no\n"
  fi
}
'

"$BIN" --mood bash --no-init-files -c '
parent_pid=$$
parent_shell_level=$SHLVL
parent_subshell_depth=$BASH_SUBSHELL

printf "identity-process="
koshkit cat < <(
  if [ "$$" = "$parent_pid" ]; then printf pid; fi
  printf ":"
  if [ "$SHLVL" = "$parent_shell_level" ]; then printf level; fi
  printf ":"
  if [ "$BASH_SUBSHELL" = "$((parent_subshell_depth + 1))" ]; then printf depth; fi
  printf ":"
  if [ "$BASHPID" != "$parent_pid" ]; then printf bashpid; fi
)
printf "\n"

printf "" | {
  printf "identity-compound="
  if [ "$$" = "$parent_pid" ]; then printf pid; fi
  printf ":"
  if [ "$SHLVL" = "$parent_shell_level" ]; then printf level; fi
  printf ":"
  if [ "$BASH_SUBSHELL" = "$((parent_subshell_depth + 1))" ]; then printf depth; fi
  printf ":"
  if [ "$BASHPID" != "$parent_pid" ]; then printf bashpid; fi
  printf "\n"
}
'

KOSH_INTERNAL_PREVIOUS_EXIT_STATUS=71 \
KOSH_INTERNAL_SHELL_PROCESS_ID=72 \
KOSH_INTERNAL_SUBSHELL_DEPTH=73 \
  "$BIN" --mood bash --no-init-files -c '
    initial_status=$?
    printf "external-state="
    if [ "$$" != 72 ]; then printf pid; fi
    printf ":"
    if [ "$initial_status" = 0 ]; then printf status; fi
    printf ":"
    if [ "$BASH_SUBSHELL" = 0 ]; then printf depth; fi
    printf "\n"
  '

startup_file=$TEST_TEMP_DIRECTORY/subshell-startup
startup_marker=$TEST_TEMP_DIRECTORY/subshell-startup-marker
test -n "$startup_file" && rm -f "$startup_file"
test -n "$startup_marker" && rm -f "$startup_marker"
printf 'printf sourced >> "$KOSH_STARTUP_MARKER"\n' > "$startup_file"
KOSH_STARTUP_MARKER=$startup_marker BASH_ENV=$startup_file \
  "$BIN" --mood bash --no-init-files -c '
    koshkit cat < <(:)
    printf "" | { :; }
  '
if [ ! -e "$startup_marker" ]; then
  printf "internal-startup=skipped\n"
fi
test -n "$startup_file" && rm -f "$startup_file"
test -n "$startup_marker" && rm -f "$startup_marker"
