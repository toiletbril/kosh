unset KOSH_FLAGS
# A mimicked script that points stdin away with an exec redirection must not
# leave the parent shell's descriptors moved, the way a fork would have
# contained it. configure does exactly this and the interactive prompt used to
# die on raw mode afterwards.
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
printf '#!/bin/bash\nexec </dev/null\necho script-ran\n' > "$dir/probe.sh"
chmod +x "$dir/probe.sh"
out=$(echo data | "$BIN" --mood bash -I -c "$dir/probe.sh; read -r line && echo got=\$line || echo stdin-lost")
rc=$?
printf '%s\n' "$out"
echo "rc=$rc"
