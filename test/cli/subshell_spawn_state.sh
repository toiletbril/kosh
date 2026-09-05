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

"$BIN" --no-init-files --no-diagnostics -c '
set --mood bash
set +u
set +o pipefail
set +o failglob
set +o extended-arithmetic
printf "runtime-marks-process="
koshkit cat < <(
  set --mood default
  [[ -o nounset ]] && printf u
  [[ -o pipefail ]] && printf p
  [[ -o failglob ]] && printf f
  [[ -o extended-arithmetic ]] && printf a
)
printf "\n"
printf "" | {
  set --mood default
  printf "runtime-marks-compound="
  [[ -o nounset ]] && printf u
  [[ -o pipefail ]] && printf p
  [[ -o failglob ]] && printf f
  [[ -o extended-arithmetic ]] && printf a
  printf "\n"
}
'

"$BIN" --mood bash --no-init-files --no-diagnostics -c '
printf "implicit-shopt-process="
koshkit cat < <(
  set --mood default
  if shopt -q expand_aliases; then printf on; else printf off; fi
)
printf "\n"
printf "" | {
  set --mood default
  printf "implicit-shopt-compound="
  if shopt -q expand_aliases; then printf on; else printf off; fi
  printf "\n"
}
'

"$BIN" --mood bash --no-init-files --no-diagnostics -c '
alias carried="printf carried"
BASH_ALIASES[written]="printf written"
printf "aliases-process="
koshkit cat < <(
  printf "%s:%s:" "${BASH_ALIASES[carried]}" "${BASH_ALIASES[written]}"
  if alias carried >/dev/null; then printf present; fi
)
printf "\n"
printf "" | {
  printf "aliases-compound=%s:%s:" \
    "${BASH_ALIASES[carried]}" "${BASH_ALIASES[written]}"
  if alias written >/dev/null; then printf "present\n"; fi
}

alias retained="printf retained"
unset BASH_ALIASES
declare -A BASH_ALIASES
BASH_ALIASES[ordinary]=value
printf "ordinary-aliases-process="
koshkit cat < <(
  printf "%s:" "${BASH_ALIASES[ordinary]}"
  if alias retained >/dev/null; then printf retained; fi
)
printf "\n"
printf "" | {
  printf "ordinary-aliases-compound=%s:" "${BASH_ALIASES[ordinary]}"
  if alias retained >/dev/null; then printf "retained\n"; fi
}
'

"$BIN" --mood bash --no-init-files --no-diagnostics -c '
cd /
pushd /tmp >/dev/null
pushd /usr >/dev/null
DIRSTACK[1]=/bin
printf "dirstack-process="
koshkit cat < <(printf "%s:%s\n" "${DIRSTACK[*]}" "$(dirs +1)")
printf "" | {
  printf "dirstack-compound=%s:%s\n" "${DIRSTACK[*]}" "$(dirs +1)"
}

unset DIRSTACK
declare -a DIRSTACK
DIRSTACK=(ordinary array)
printf "ordinary-dirstack-process="
koshkit cat < <(printf "%s:%s\n" "${DIRSTACK[*]}" "$(dirs +1)")
printf "" | {
  printf "ordinary-dirstack-compound=%s:%s\n" \
    "${DIRSTACK[*]}" "$(dirs +1)"
}
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

"$BIN" --mood bash --no-init-files --no-diagnostics -c '
f()
{
  local outer=parent
  printf "local-process="
  koshkit cat < <(
    local inner=child
    printf "%s:%s\n" "$outer" "$inner"
  )
  printf x | {
    local inner=child
    printf "local-compound=%s:%s\n" "$outer" "$inner"
  }
}
f
'

"$BIN" --mood bash --no-init-files --no-diagnostics -c '
shopt -s extdebug
outer() { inner "inner-one" "inner two"; }
inner()
{
  printf "stack-process="
  koshkit cat < <(
    if [ "${FUNCNAME[*]}" = "inner outer" ] &&
       [ -n "${BASH_LINENO[*]}" ] && [ -n "${BASH_SOURCE[*]}" ] &&
       [ "${BASH_ARGC[*]}" = "2 2 0" ] &&
       [ "${BASH_ARGV[*]}" = "inner two inner-one outer two outer-one" ]
    then
      printf yes
    else
      printf no
    fi
  )
  printf "\n"
  printf x | {
    if [ "${FUNCNAME[*]}" = "inner outer" ] &&
       [ -n "${BASH_LINENO[*]}" ] && [ -n "${BASH_SOURCE[*]}" ] &&
       [ "${BASH_ARGC[*]}" = "2 2 0" ] &&
       [ "${BASH_ARGV[*]}" = "inner two inner-one outer two outer-one" ]
    then
      printf "stack-compound=yes\n"
    else
      printf "stack-compound=no\n"
    fi
  }
}
outer "outer-one" "outer two"
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

"$BIN" --no-init-files --no-diagnostics -c '
value=before
eval "value=after" | koshkit cat
printf "pipeline-state=%s\n" "$value"
'

pipeline_output=$("$BIN" --no-init-files --no-diagnostics -c \
  "set +o pipefail; eval 'koshkit seq 1 100000' | koshkit head -n 1")
pipeline_status=$?
printf "pipeline-output=%s\n" "$pipeline_output"
printf "pipeline-status=%s\n" "$pipeline_status"
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
