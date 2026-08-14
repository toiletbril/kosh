# shellcheck disable=SC2154
unset KOSH_FLAGS
# A warning from a sourced file prints the chain that reached it, the same
# trace frames an error shows, while a warning in the typed line stays a
# single report.
# Fixed paths rather than mktemp, so the trace caret width, which spans the real
# source path, is the same length on every platform and the golden stays
# portable. mktemp yields a longer path on macOS than on Linux.
mkdir -p "$TEST_TEMP_DIRECTORY"
test_temp_directory=$(cd "$TEST_TEMP_DIRECTORY" && pwd -P)
outer=$test_temp_directory/wscouter
inner=$test_temp_directory/wscinner
cat > "$outer" <<EOF
outer=1
. $inner
EOF
cat > "$inner" <<'EOF'
inner=1
[[ x = "$UNSET_CHAIN_FIRST" ]]
[[ x = "$UNSET_CHAIN_SECOND" ]]
EOF
"$BIN" -WWW -c ". $outer" 2>&1 | sed "s|$outer|OUTER|; s|$inner|INNER|" | ./normalize-trace.sh "$BIN"
cat > "$inner" <<'EOF'
echo "$MIMIC_CHAIN_FIRST"
echo "$MIMIC_CHAIN_SECOND"
EOF
chmod +x "$inner"
"$BIN" --mood bash -WWW -c '"$1"' trace-driver "$inner" 2>&1 |
    sed "s|$inner|INNER|" | ./normalize-trace.sh "$BIN"
"$BIN" -WWW -c '[[ x = "$UNSET_FLAT" ]]' 2>&1 | grep -Ec 'trace:'
[ -n "$test_temp_directory" ] && rm -f "$outer" "$inner"
echo "rc=$?"
