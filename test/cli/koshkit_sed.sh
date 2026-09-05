unset KOSH_FLAGS

echo "--- anchored global substitution ---"
"$BIN" -c "printf 'ab\\n' | koshkit sed 's/^/x/g'"

echo "--- adjacent empty global match ---"
"$BIN" -c "printf 'ab\\n' | koshkit sed 's/a*/x/g'"

echo "--- unterminated input ---"
"$BIN" -c "printf a | koshkit sed 's/a/b/'; printf '<end>\\n'"

echo "--- script from standard input ---"
sed_data=$TEST_TEMP_DIRECTORY/koshkit-sed-data
printf 'alpha\n' > "$sed_data"
printf 's/alpha/ALPHA/\n' | \
  "$BIN" -c 'koshkit sed -f - "$1"' sed-test "$sed_data"
