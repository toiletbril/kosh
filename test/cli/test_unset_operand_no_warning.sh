unset KOSH_FLAGS
# A test or [ command probes presence or emptiness, so an unset variable operand
# is the question being asked, not a mistake. The unset-variable warning is
# suppressed across the operand expansion, while a plain expansion elsewhere
# still warns, and the suppression does not leak past the command.
echo "== test -z on an unset var is silent:"
"$BIN" -W -c 'test -z "$NOPE" && echo empty' 2>&1
echo "== [ -z on an unset var is silent:"
"$BIN" -W -c '[ -z "$NOPE" ] && echo bracket-empty' 2>&1
echo "== test equality with an unset var is silent:"
"$BIN" -W -c 'test "$NOPE" = x || echo not-x' 2>&1
echo "== a plain expansion still warns (count):"
"$BIN" -W -c '# shellcheck disable=SC2154
echo "[$NOPE]"' 2>&1 | grep -c "is not set"
echo "== the suppression does not leak past the test (count):"
"$BIN" -W -c '# shellcheck disable=SC2154
test -z "$A"; echo "[$B]"' 2>&1 | grep -c "is not set"
