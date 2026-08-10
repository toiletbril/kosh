unset KOSH_FLAGS
if ! command -v timeout >/dev/null 2>&1; then echo "exit=1"; exit 0; fi
timeout --preserve-status -s INT -k 3 1 "$BIN" --mood bash -c 'while true; do for ((j=0;j<100000;j++)); do :; done; done' </dev/null >/dev/null 2>&1
echo "exit=$?"
