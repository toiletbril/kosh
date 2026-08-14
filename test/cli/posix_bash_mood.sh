unset KOSH_FLAGS
# --posix selects the bash-posix mood, not the dash-like sh mood, so the bash
# identity is seeded and the bash grammar stays on. [[ still works, arrays still
# work, and BASH_VERSION is set so a bash rc detects it.
echo "== --posix keeps [[ working:"
"$BIN" --posix -c '[[ x == x ]] && echo bracket-works'
echo "== --posix seeds BASH_VERSION:"
"$BIN" --posix -c 'echo "BASH_VERSION=${BASH_VERSION:-unset}"'
echo "== --posix seeds BASH_VERSINFO:"
"$BIN" --posix -c \
  'printf "BASH_VERSINFO=%s.%s.%s.%s\n" "${BASH_VERSINFO[@]:0:4}"'
echo "== --posix reports the bash-posix mood:"
"$BIN" --posix -c 'set --mood'
echo "== --mood bash-posix keeps [[ working:"
"$BIN" --mood bash-posix -c '[[ x == x ]] && echo bracket-works'
echo "== --posix does not enter the sh mood:"
"$BIN" --posix -c 'arr=(a b c); echo "${arr[1]}"'
