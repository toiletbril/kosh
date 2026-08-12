# shellcheck disable=SC2154
unset KOSH_FLAGS
# A script run as a command hits ENOEXEC and is mimicked in-process, so a parse
# error in it locates against the script file and name, not the command line the
# way it used to caret an unrelated column of the typed path.
dir=$(mktemp -d)
canonical_dir=$(cd "$dir" && pwd -P)
trap 'rm -rf "$dir"' EXIT
printf '{\n  echo hi\n' > "$dir/unterminated"
chmod +x "$dir/unterminated"
"$BIN" --no-traces -c "$dir/unterminated" 2>&1 | sed 's|\\|/|g' | sed "s|$dir|TMPDIR|g"
echo "rc=$?"

printf 'printf "incremental reached\\n"\nexit 0\n(\n' > "$dir/incremental"
chmod +x "$dir/incremental"
"$BIN" --no-traces -c "$dir/incremental"
echo "incremental rc=$?"

printf '%s\n' \
    '"$1" -c '\''printf "complete first\\n"'\''' \
    'printf "complete second\\n"' > "$dir/complete"
chmod +x "$dir/complete"
"$BIN" --no-traces -I -c "$dir/complete '$BIN'"

printf 'trap '\''echo trap-status=$?'\'' EXIT\nexec >"$1" 2>&1\n(\n' > "$dir/error-state"
chmod +x "$dir/error-state"
"$BIN" --no-traces -c "$dir/error-state '$dir/error-state-output'" 2> "${TEST_NULL_DEVICE:-/dev/null}"
error_state_status=$?
sed 's|\\|/|g' "$dir/error-state-output" | sed "s|$canonical_dir|TMPDIR|g" | sed "s|$dir|TMPDIR|g"
echo "error-state rc=$error_state_status"

printf '\0binary\n' > "$dir/binary"
chmod +x "$dir/binary"
(cd "$dir" && "$BIN" --no-traces -c ./binary) > "$dir/binary-output" 2>&1
binary_status=$?
sed 's|\\|/|g' "$dir/binary-output" | sed "s|$canonical_dir|TMPDIR|g" | sed "s|$dir|TMPDIR|g"
echo "binary rc=$binary_status"

printf '#!/bin/sh\n"$0"\n' > "$dir/recurse"
chmod +x "$dir/recurse"
out=$(KOSH_FLAGS= "$BIN" --mood bash -I "$dir/recurse" 2>&1)
recursive_status=$?
printf '%s\n' "$out" |
    sed -e "s#$canonical_dir#TMPDIR#g" -e "s#$dir#TMPDIR#g" \
        -e '/trace:/,/\^~*$/d'
echo "recursive rc=$recursive_status"
