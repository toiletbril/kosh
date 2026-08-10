unset KOSH_FLAGS
# The KOSH_ANSI_ color variables expose the SGR escapes a prompt template uses.
# The harness captures a pipe, so stdout_wants_color is false and a recognized
# name reads back empty. The ${name-MISSING} form still separates a recognized
# name, which is set and reads empty, from an unknown name, which is unset and
# takes the default, so the probe locks the table membership without a terminal.
names="BLACK RED GREEN YELLOW BLUE MAGENTA CYAN WHITE \
BRIGHT_BLACK BRIGHT_RED BRIGHT_GREEN BRIGHT_YELLOW \
BRIGHT_BLUE BRIGHT_MAGENTA BRIGHT_CYAN BRIGHT_WHITE BOLD DIM RESET BOGUS"
for name in $names; do
  "$BIN" -c "printf '%-16s [%s]\n' KOSH_ANSI_$name \"\${KOSH_ANSI_$name-MISSING}\""
done
# A stored assignment wins over the computed escape.
"$BIN" -c 'KOSH_ANSI_RED=stored; printf "stored=[%s]\n" "$KOSH_ANSI_RED"'
# The recognition holds in the sh mood the same as the default mood.
"$BIN" --mood sh -c 'printf "sh_green=[%s]\n" "${KOSH_ANSI_GREEN-MISSING}"'
# An unknown name is unset, so the strict report fires under nounset.
"$BIN" -c 'set -u; echo "${KOSH_ANSI_NOPE}"'
echo "rc=$?"
