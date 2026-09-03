# shellcheck disable=SC2154
unset KOSH_FLAGS

"$BIN" -c \
  'koshkit timeout -p -s INT -k 5s 0.1s "$1" -c "$2" arithmetic 3; echo "arithmetic-exit=$?"' \
  signaler "$BIN" 'n=$1; echo $((n ** 100000000))' 2>&1

if ! command -v timeout >/dev/null 2>&1; then echo "exit=1"; exit 0; fi
timeout --preserve-status -s INT -k 3 1 "$BIN" --mood bash -c 'while true; do for ((j=0;j<100000;j++)); do :; done; done' </dev/null >/dev/null 2>&1
echo "exit=$?"
