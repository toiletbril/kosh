# shellcheck disable=SC2154
unset KOSH_FLAGS
# A warning from a sourced file prints the chain that reached it, the same
# trace frames an error shows, while a warning in the typed line stays a
# single report.
# Fixed paths rather than mktemp, so the trace caret width, which spans the real
# source path, is the same length on every platform and the golden stays
# portable. mktemp yields a longer path on macOS than on Linux.
outer=$TEST_TEMP_DIRECTORY/wscouter
inner=$TEST_TEMP_DIRECTORY/wscinner
mkdir -p "$TEST_TEMP_DIRECTORY"
cat > "$outer" <<EOF
outer=1
. $inner
EOF
cat > "$inner" <<'EOF'
inner=1
[[ x = "$UNSET_CHAIN" ]]
EOF
"$BIN" -WWW -c ". $outer" 2>&1 | sed "s|$outer|OUTER|; s|$inner|INNER|" | ./normalize-trace.sh "$BIN"
"$BIN" -WWW -c '[[ x = "$UNSET_FLAT" ]]' 2>&1 | grep -Ec 'trace:'
rm -f "$outer" "$inner"
echo "rc=$?"
