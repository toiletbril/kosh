directory=$(mktemp -d)
cleanup()
{
  if [ -n "$directory" ]; then
    /bin/rm -rf "$directory"
  fi
}
trap cleanup EXIT

printf 'if [ "${profile_loaded-}" = yes ]; then printf login-bashrc-marker; else printf bashrc-marker; fi\n' \
  > "$directory/.bashrc"
printf 'profile_loaded=yes\nprintf profile-marker\n' \
  > "$directory/.bash_profile"
printf 'printf custom-marker\n' > "$directory/custom"
printf 'printf init-marker\n' > "$directory/init"
printf 'printf tilde-marker\n' > "$directory/.custom"

norc=ok
rcfile=ok
init_file=ok
last_alias=ok
last_rcfile=ok
login=ok
tilde=ok
privileged_rc=ok
privileged_env=ok
unreadable_rc=ok
script_mode=
if script -qec true /dev/null >/dev/null 2>&1; then
  script_mode=gnu
elif script -q /dev/null /usr/bin/true >/dev/null 2>&1; then
  script_mode=bsd
fi
run_interactive()
{
  if [ "$script_mode" = gnu ]; then
    script -qec "$1" /dev/null 2>/dev/null
  else
    script -q /dev/null /bin/sh -c "$1" 2>/dev/null
  fi
}
if [ -n "$script_mode" ]; then
  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --norc --rcfile $directory/custom -i")
  case "$output" in *bashrc-marker*|*custom-marker*) norc=broken ;; esac

  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --rcfile $directory/custom -i")
  case "$output" in *custom-marker*) ;; *) rcfile=broken ;; esac

  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --init-file $directory/init -i")
  case "$output" in *init-marker*) ;; *) init_file=broken ;; esac

  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --rcfile $directory/custom --init-file $directory/init -i")
  case "$output" in *init-marker*) ;; *) last_alias=broken ;; esac

  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --init-file $directory/init --rcfile $directory/custom -i")
  case "$output" in *custom-marker*) ;; *) last_rcfile=broken ;; esac

  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --login --rcfile $directory/custom -i")
  case "$output" in
    *profile-marker*)
      case "$output" in *login-bashrc-marker*|*custom-marker*) login=broken ;; esac
      ;;
    *) login=broken ;;
  esac

  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --init-file '~/.custom' -i")
  case "$output" in *tilde-marker*) ;; *) tilde=broken ;; esac

  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --privileged --rcfile $directory/custom -i")
  case "$output" in *custom-marker*) ;; *) privileged_rc=broken ;; esac

  output=$(printf 'exit\n' |
    HOME="$directory" ENV="$directory/env" NO_COLOR=1 run_interactive \
      "$BIN --mood sh --privileged -i")
  case "$output" in *env:*) privileged_env=broken ;; esac

  output=$(printf 'exit\n' |
    HOME="$directory" NO_COLOR=1 run_interactive \
      "$BIN --mood bash --rcfile $directory -i")
  case "$output" in
    *"Unable to read startup file"*) ;;
    *) unreadable_rc=broken ;;
  esac
else
  norc=ok
  rcfile=ok
  init_file=ok
  last_alias=ok
  last_rcfile=ok
  login=ok
  tilde=ok
  privileged_rc=ok
  privileged_env=ok
  unreadable_rc=ok
fi
printf 'norc=%s rcfile=%s init-file=%s aliases=%s/%s login=%s tilde=%s privileged-rc=%s privileged-env=%s unreadable-rc=%s\n' \
  "$norc" "$rcfile" "$init_file" "$last_alias" "$last_rcfile" "$login" "$tilde" \
  "$privileged_rc" "$privileged_env" "$unreadable_rc"

output=ok
if [ "${IS_NONDEBUG_BUILD:-0}" != 1 ]; then
  "$BIN" --mood bash -i -X debug \
    --debug-logging-file "$directory/completion.log" --rcfile /dev/null \
    </dev/null >/dev/null 2>&1 || :
  grep -F 'bootstrapping the bash programmable completion' \
    "$directory/completion.log" >/dev/null 2>&1 || output=broken
  "$BIN" --mood bash -i --no-completion -X debug \
    --debug-logging-file "$directory/no-completion.log" --rcfile /dev/null \
    </dev/null >/dev/null 2>&1 || :
  if grep -F 'bootstrapping the bash programmable completion' \
    "$directory/no-completion.log" >/dev/null 2>&1; then
    output=broken
  fi
fi
printf 'no-completion-bootstrap=%s\n' "$output"

printf 'printf env:' > "$directory/env"
printf 'printf expanded:' > "$directory/env0"
printf 'printf body' > "$directory/script"

output=$(BASH_ENV="$directory/env" "$BIN" --mood bash -c 'printf body')
printf 'bash-env-command=%s\n' "$output"
output=$(printf 'printf body' |
  BASH_ENV="$directory/env" "$BIN" --mood bash -s)
printf 'bash-env-stdin=%s\n' "$output"
output=$(BASH_ENV="$directory/env" "$BIN" --mood bash "$directory/script")
printf 'bash-env-script=%s\n' "$output"
output=$(ROOT="$directory" \
  BASH_ENV='$ROOT/env$((1 - 1))$(printf "")' \
  "$BIN" --mood bash -c 'printf body')
printf 'bash-env-expanded=%s\n' "$output"
output=$(HOME="$directory" BASH_ENV='~/env' \
  "$BIN" --mood bash -c 'printf body')
printf 'bash-env-tilde=%s\n' "$output"
output=$(BASH_ENV="$directory/env" "$BIN" --mood bash-posix -c 'printf body')
printf 'bash-env-posix=%s\n' "$output"
output=$(BASH_ENV="$directory/env" "$BIN" --mood kosh -c 'printf body')
printf 'bash-env-kosh=%s\n' "$output"
output=$(BASH_ENV="$directory/env" "$BIN" --mood bash -Q -c 'printf body')
printf 'bash-env-clean=%s\n' "$output"
output=$(BASH_ENV="$directory/env" \
  "$BIN" --mood bash --privileged -c 'printf body')
printf 'bash-env-privileged=%s\n' "$output"
printf 'set -p\nprintf profile:' > "$directory/.bash_profile"
output=$(HOME="$directory" BASH_ENV="$directory/env" \
  "$BIN" --mood bash --login -c 'printf body')
printf 'bash-env-runtime-privileged=%s\n' "$output"
output=$(BASH_ENV="$directory" "$BIN" --mood bash -c 'printf body' 2>&1)
if [ "${OS-}" = Windows_NT ]; then
  output=ok
else
  case "$output" in
    *"Unable to read startup file"*"Is a directory"*"body"*) output=ok ;;
    *) output=broken ;;
  esac
fi
printf 'bash-env-unreadable=%s\n' "$output"
printf x > "$directory/not-directory"
output=$(BASH_ENV="$directory/not-directory/child" \
  "$BIN" --mood bash -c 'printf body' 2>&1)
if [ "${OS-}" = Windows_NT ]; then
  output=ok
else
  case "$output" in
    *"Not a directory"*"body"*) output=ok ;;
    *) output=broken ;;
  esac
fi
printf 'bash-env-not-directory=%s\n' "$output"
mkdir "$directory/blocked"
printf 'printf leaked' > "$directory/blocked/env"
if [ "${OS-}" = Windows_NT ] || [ "$(id -u)" = 0 ]; then
  output=ok
else
  chmod 000 "$directory/blocked"
  output=$(BASH_ENV="$directory/blocked/env" \
    "$BIN" --mood bash -c 'printf body' 2>&1)
  chmod 700 "$directory/blocked"
  case "$output" in
    *"Permission denied"*"body"*) output=ok ;;
    *) output=broken ;;
  esac
fi
printf 'bash-env-permission=%s\n' "$output"

output=$("$BIN" --mood bash --privileged -c \
  'case "$-" in *p*) printf flag ;; esac; [[ -o privileged ]] && printf option; case ":$SHELLOPTS:" in *:privileged:*) printf shellopts ;; esac')
printf 'privileged-identity=%s\n' "$output"
output=$("$BIN" --mood bash -c \
  'set -p; case "$-" in *p*) printf on ;; esac; set +p; case "$-" in *p*) printf leaked ;; *) printf off ;; esac')
printf 'runtime-privileged=%s\n' "$output"

printf 'PATH="$TEST_SYSTEM_PATH"\ncd ..\nprintf "startup=%%s:" "$?"\n' \
  > "$directory/restricted-env"
output=$(BASH_ENV="$directory/restricted-env" \
  "$BIN" --mood bash --restricted -c 'printf body')
printf 'restricted-startup=%s\n' "$output"

printf '%s\n' \
  'case "$-" in *r*) printf r ;; *) printf no-r ;; esac' \
  'shopt -q restricted_shell; printf ":%s:" "$?"' \
  'cd ..; printf "%s" "$?"' > "$directory/restricted-state-env"
output=$(BASH_ENV="$directory/restricted-state-env" \
  "$BIN" --mood bash --restricted -c ':' 2>/dev/null)
printf 'restricted-startup-state=%s\n' "$output"

printf 'set -r\ncd ..\n' > "$directory/set-r-env"
output=$(cd "$directory" &&
  BASH_ENV="$directory/set-r-env" \
    "$BIN" --mood bash -c 'printf "%s" "$PWD"' 2>/dev/null)
if [ "$output" = / ]; then
  printf 'startup-set-r=changed\n'
else
  printf 'startup-set-r=blocked\n'
fi

output=$("$BIN" --mood bash --restricted -c \
  'case "$-" in *r*) printf flags ;; esac; shopt -q restricted_shell && printf shopt')
printf 'restricted-identity=%s\n' "$output"

output=$("$BIN" --mood bash -c \
  'set -r; case "$-" in *r*) printf flags ;; esac; shopt -q restricted_shell || printf no-shopt')
printf 'runtime-restricted=%s\n' "$output"

output=$("$BIN" --mood bash -c \
  'shopt -s restricted_shell; printf "%s:" "$?"; shopt -u restricted_shell; printf "%s:" "$?"; shopt -q restricted_shell; printf "%s" "$?"')
printf 'restricted-shopt-normal=%s\n' "$output"
output=$("$BIN" --mood bash --restricted -c \
  'shopt -u restricted_shell; printf "%s:" "$?"; shopt -q restricted_shell; printf "%s" "$?"')
printf 'restricted-shopt-fixed=%s\n' "$output"

if [ "${OS-}" = Windows_NT ]; then
  "$BIN" -c 'koshkit cp "$1" "$2"' \
    test-copy "$BIN" "$directory/rbash.exe"
  rbash=$directory/rbash.exe
else
  ln -s "$BIN" "$directory/rbash"
  rbash=$directory/rbash
fi
output=$("$rbash" -c \
  'case "$-" in *r*) printf flags ;; esac; shopt -q restricted_shell && printf shopt')
printf 'rbash-identity=%s\n' "$output"

separator=
for name in SHELL PATH HISTFILE ENV BASH_ENV; do
  "$BIN" --mood bash --restricted -c "$name=blocked" >/dev/null 2>&1
  printf '%s%s=%s' "$separator" "$name" "$?"
  separator=' '
done
printf '\n'

for command in \
  'PATH[0]=blocked' \
  'PATH=(blocked)' \
  'declare -a PATH=(blocked)' \
  'declare -A PATH=([x]=blocked)' \
  'printf -v "PATH[0]" blocked' \
  'read -a PATH <<EOF
blocked
EOF' \
  'mapfile -t PATH <<EOF
blocked
EOF' \
  'unset "PATH[0]"' \
  'f() { local -a PATH; }; f' \
  'declare -i PATH' \
  'declare +i PATH' \
  'export PATH=blocked'
do
  "$BIN" --mood bash --restricted -c "$command" >/dev/null 2>&1
  printf '%s ' "$?"
done
"$BIN" --mood bash --restricted -c \
  'export PATH; readonly -p | grep -q "declare -r PATH="' >/dev/null 2>&1
printf '%s\n' "$?"

output=$("$BIN" --mood bash --restricted -c \
  'old=$PATH; export -n PATH; status=$?; test "$PATH" = "$old"; printf "%s:%s" "$status" "$?"')
printf 'unexport=%s ' "$output"
"$BIN" --mood bash --restricted -c \
  'export -n PATH=blocked' >/dev/null 2>&1
printf 'unexport-value=%s\n' "$?"
output=$("$BIN" --mood bash -c \
  'export RANDOM; export -n RANDOM; first=$RANDOM; result=frozen; for value in $RANDOM $RANDOM $RANDOM $RANDOM $RANDOM; do if [[ $value != "$first" ]]; then result=dynamic; break; fi; done; printf "%s" "$result"')
printf 'unexport-dynamic=%s ' "$output"
output=$("$BIN" --mood bash -c \
  'f() { :; }; export -f f; export -nf f; env | grep -q "^BASH_FUNC_f%%="; printf "%s:" "$?"; f; printf "%s" "$?"')
printf 'unexport-function=%s ' "$output"
"$BIN" --mood bash -c "export -n 'a-b'" >/dev/null 2>&1
printf 'unexport-invalid=%s\n' "$?"
output=$("$BIN" --mood bash --restricted -c \
  'declare -p PATH; export -n PATH; declare -p PATH')
case "$output" in
  'declare -rx PATH='*'
declare -r PATH='*) output=ok ;;
  *) output=broken ;;
esac
printf 'restricted-declare=%s\n' "$output"

marker="$directory/array-substitution"
"$BIN" --mood bash --restricted -c \
  "PATH=(\$(touch '$marker'))" >/dev/null 2>&1
if [ -e "$marker" ]; then
  printf 'array-substitution=ran\n'
else
  printf 'array-substitution=blocked\n'
fi

"$BIN" --mood bash --restricted -c '/bin/echo blocked' >/dev/null 2>&1
printf 'slash=%s ' "$?"
"$BIN" --mood bash --restricted -c \
  'command=/bin/echo; "$command" blocked' >/dev/null 2>&1
printf 'expanded-slash=%s ' "$?"
"$BIN" --mood bash --restricted -c 'cd /' >/dev/null 2>&1
printf 'cd=%s ' "$?"
"$BIN" --mood bash --restricted -c ". '$directory/script'" >/dev/null 2>&1
printf 'source=%s ' "$?"
"$BIN" --mood bash --restricted -c 'command -p true' >/dev/null 2>&1
printf 'command-p=%s ' "$?"
"$BIN" --mood bash --restricted -c 'command -v "$BIN"' >/dev/null 2>&1
printf 'command-v=%s ' "$?"
"$BIN" --mood bash --restricted -c 'command -V "$BIN"' >/dev/null 2>&1
printf 'command-V=%s ' "$?"
"$BIN" --mood bash --restricted -c 'exec true' >/dev/null 2>&1
printf 'exec=%s\n' "$?"

"$BIN" --mood bash --restricted -c "echo blocked >'$directory/out'" \
  >/dev/null 2>&1
printf 'truncate=%s ' "$?"
"$BIN" --mood bash --restricted -c "echo blocked >>'$directory/out'" \
  >/dev/null 2>&1
printf 'append=%s ' "$?"
"$BIN" --mood bash --restricted -c "echo blocked <>'$directory/out'" \
  >/dev/null 2>&1
printf 'readwrite=%s ' "$?"
"$BIN" --mood bash --restricted -c "echo blocked >&'$directory/out'" \
  >/dev/null 2>&1
printf 'both=%s ' "$?"
"$BIN" --mood bash --restricted -c "echo blocked >|'$directory/out'" \
  >/dev/null 2>&1
printf 'override=%s ' "$?"
"$BIN" --mood bash --restricted -c "echo blocked &>'$directory/out'" \
  >/dev/null 2>&1
printf 'ampersand=%s ' "$?"
"$BIN" --mood bash --restricted -c "echo blocked &>>'$directory/out'" \
  >/dev/null 2>&1
printf 'ampersand-append=%s ' "$?"
"$BIN" --mood bash --restricted -c 'echo allowed 2>&1' >/dev/null 2>&1
printf 'fd-dup=%s\n' "$?"

"$BIN" --mood bash --restricted -c \
  "enable -f '$directory/script' true" >/dev/null 2>&1
printf 'enable=%s ' "$?"
"$BIN" --mood bash --restricted -c 'enable -d true' >/dev/null 2>&1
printf 'enable-d=%s ' "$?"
"$BIN" --mood bash --restricted -c \
  "hash -p '$directory/script' probe" >/dev/null 2>&1
printf 'hash=%s ' "$?"
"$BIN" --mood bash --restricted -c \
  "history -r '$directory/script'" >/dev/null 2>&1
printf 'history=%s ' "$?"
"$BIN" --mood bash --restricted -c \
  "history -w '$directory/history'" >/dev/null 2>&1
printf 'history-write=%s ' "$?"
"$BIN" --mood bash --restricted -c 'set +r' >/dev/null 2>&1
printf 'unset=%s\n' "$?"

output=$("$BIN" --mood bash -c \
  '(set -r); cd /; printf "cd=%s" "$?"' 2>/dev/null)
if [ "${OS-}" = Windows_NT ]; then
  output=$("$BIN" --mood bash -c \
    '(set -r); cd .; printf "cd=%s" "$?"' 2>/dev/null)
fi
printf 'subshell-restore=%s\n' "$output"
output=$("$BIN" --mood bash -c \
  'declare -A PATH=([x]=old); f() { local PATH=scalar; set -r; }; f; printf "%s" "${PATH[x]}"' \
  2>/dev/null)
printf 'associative-local-restore=%s\n' "$output"
output=$("$BIN" --mood bash -c \
  'declare -a PATH; PATH[9]=old; f() { local PATH=scalar; set -r; }; f; printf "%s" "${PATH[9]}"' \
  2>/dev/null)
printf 'sparse-local-restore=%s\n' "$output"

mkdir "$directory/z-target"
printf '%s\t1\t1\n' "$directory/z-target" > "$directory/z-store"
output=$(KOSH_DIRECTORY_HISTORY="$directory/z-store" \
  "$BIN" --mood bash --restricted -c \
  'before=$PWD; z z-target; status=$?; test "$PWD" = "$before"; printf "status=%s cwd=%s" "$status" "$?"' \
  2>/dev/null)
printf 'z=%s\n' "$output"

printf '#!/bin/bash\ncd ..\nprintf "script-cd=%%s" "$?"\n' \
  > "$directory/restricted-script"
chmod +x "$directory/restricted-script"
output=$(env -u PATH \
  "$TEST_PATH_ENVIRONMENT_NAME=$directory${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
  "$BIN" --mood bash --restricted -I -c \
  'restricted-script')
printf 'executed=%s\n' "$output"
output=$(env -u PATH \
  "$TEST_PATH_ENVIRONMENT_NAME=$directory${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
  "$BIN" --mood bash --restricted -c \
  '. restricted-script' 2>/dev/null)
printf 'sourced=%s\n' "$output"
