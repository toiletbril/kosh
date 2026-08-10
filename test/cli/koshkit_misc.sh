# The remaining utilities, sleep with a zero and a bad duration, env applying an
# assignment and running a command, and the pkill and killall error paths. The
# process matchers use a name that matches nothing, so no real process is
# signalled.
unset KOSH_FLAGS

echo "--- sleep zero ---"
"$BIN" -c 'koshkit sleep 0'
echo "rc=$?"

echo "--- sleep accepts representable subnormal values ---"
"$BIN" -c 'koshkit sleep 4.9406564584124654e-324'
echo "minimum=$?"
"$BIN" -c 'koshkit sleep 1e-323'
echo "subnormal=$?"
"$BIN" -c 'koshkit sleep 1e-4000' 2>&1
echo "underflow=$?"

echo "--- sleep bad duration ---"
"$BIN" -c 'koshkit sleep abc' 2>&1
echo "rc=$?"

echo "--- env runs the command ---"
"$BIN" -c 'koshkit env X=1 koshkit seq 1'
echo "rc=$?"

echo "--- env applies the assignment ---"
"$BIN" -c 'koshkit env KOSHKIT_TESTVAR=present | koshkit grep KOSHKIT_TESTVAR'
bare_pipeline_output=$("$BIN" --mood sh --enable-koshkit -c \
    'PATH=; seq 1 100000 | head -n 1') || exit 1
[ "$bare_pipeline_output" = 1 ] || exit 1
"$BIN" -c \
    "pipeline_value='\$pipeline'; koshkit env \"PIPELINE_LITERAL=\$pipeline_value\" | koshkit grep 'PIPELINE_LITERAL=\$pipeline'" \
    > "${TEST_NULL_DEVICE:-/dev/null}" || exit 1
large_pipeline_file=$TEST_TEMP_DIRECTORY/large-pipeline
large_pipeline_count=$(
    "$BIN" -c 'koshkit seq 1 100000 > "$1"; koshkit cat "$1" | koshkit wc -c' \
        pipeline-test "$large_pipeline_file"
) || exit 1
[ "$large_pipeline_count" -gt 100000 ] || exit 1
large_builtin_count=$(
    "$BIN" -c "printf '%100000s' x | koshkit wc -c"
) || exit 1
[ "$large_builtin_count" -eq 100000 ] || exit 1

echo "--- pkill with no pattern ---"
"$BIN" -c 'koshkit pkill' 2>&1
echo "rc=$?"

echo "--- pkill with no match ---"
"$BIN" -c 'koshkit pkill no_such_process_xyz_123'
echo "rc=$?"

echo "--- killall with no match ---"
"$BIN" -c 'koshkit killall no_such_process_xyz_123' 2>&1
echo "rc=$?"

echo "--- kill is a builtin, not a koshkit utility ---"
"$BIN" -c 'koshkit kill' 2>&1
echo "rc=$?"

echo "--- kill with a non-numeric pid ---"
"$BIN" -c 'kill notapid' 2>&1
echo "rc=$?"

echo "--- ps prints the header ---"
"$BIN" --mood sh -c 'koshkit ps | koshkit head -n 1'
echo "rc=$?"

echo "--- list prints the utility count ---"
"$BIN" -c 'koshkit --list | koshkit wc -l'
