#!/bin/sh

printf '== stored candidates:\n'
"$BIN" -M bash -c 'compgen -V stored -W "alpha beta apex" -- a
printf "status=%s\n" "$?"
declare -p stored'

printf '== nothing reaches standard output:\n'
"$BIN" -M bash -c 'printf "captured=[%s]\n" "$(compgen -V stored -W "alpha beta" -- a)"
compgen -V stored -W "alpha beta" -- a
declare -p stored'

printf '== the store replaces every earlier element:\n'
"$BIN" -M bash -c 'stored=(old keep)
compgen -V stored -W "alpha" -- a
declare -p stored'

printf '== a sparse element does not survive the store:\n'
"$BIN" -M bash -c 'stored[7]=far
compgen -V stored -W "alpha" -- a
declare -p stored
printf "count=%s\n" "${#stored[@]}"'

printf '== a scalar is promoted to an indexed array:\n'
"$BIN" -M bash -c 'stored=one
compgen -V stored -W "alpha" -- a
declare -p stored'

printf '== no match leaves an empty array and status 1:\n'
"$BIN" -M bash -c 'stored=(old)
compgen -V stored -W "alpha beta" -- z
printf "status=%s\n" "$?"
declare -p stored'

printf '== no generator leaves an empty array and status 1:\n'
"$BIN" -M bash -c 'compgen -V stored
printf "status=%s\n" "$?"
declare -p stored'

printf '== duplicates are preserved:\n'
"$BIN" -M bash -c 'compgen -V stored -W "alpha alpha beta" -- a
declare -p stored'

printf '== the last -V wins:\n'
"$BIN" -M bash -c 'compgen -V first -V last -W "alpha" -- a
declare -p last
declare -p first 2>/dev/null
printf "first-status=%s\n" "$?"'

printf '== the prefix and the suffix reach both the array and the output:\n'
"$BIN" -M bash -c 'compgen -V stored -P pre -S suf -W "alpha apex" -- a
declare -p stored
compgen -P pre -S suf -W "alpha apex" -- a'

printf '== the filter sees the candidate without its prefix:\n'
"$BIN" -M bash -c 'compgen -P pre -X "pre*" -W "alpha apex" -- a
printf "raw-status=%s\n" "$?"
compgen -P pre -X "a*" -W "alpha apex" -- a
printf "prefixed-status=%s\n" "$?"'

printf '== a local target keeps the store inside the function:\n'
"$BIN" -M bash -c 'wrapper()
{
  local stored=(caller)
  compgen -V stored -W "alpha" -- a
  declare -p stored
}
wrapper
declare -p stored 2>/dev/null
printf "outer-status=%s\n" "$?"'

printf '== the kosh mood stores as well:\n'
"$BIN" -c 'compgen -V stored -W "alpha" -- a
declare -p stored'

printf '== an invalid name is refused:\n'
"$BIN" -M bash -c 'compgen -V "stored[0]" -W "alpha" -- a
printf "status=%s\n" "$?"'

printf '== an associative target is refused:\n'
"$BIN" -M bash -c 'declare -A stored
compgen -V stored -W "alpha" -- a
printf "status=%s\n" "$?"'

printf '== a read only target is refused:\n'
"$BIN" -M bash -c 'readonly stored=1
compgen -V stored -W "alpha" -- a
printf "status=%s\n" "$?"'

printf '== a missing option value is a usage error:\n'
"$BIN" -M bash -c 'compgen -W "alpha" -V'
printf 'status=%s\n' "$?"
