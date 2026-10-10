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

sed_first=$TEST_TEMP_DIRECTORY/koshkit-sed-first
sed_last=$TEST_TEMP_DIRECTORY/koshkit-sed-last
printf 'first\nsecond\n' > "$sed_first"
printf 'third\n' > "$sed_last"

echo "--- multiple data files with a missing operand ---"
"$BIN" -c \
  'koshkit sed -n "2,3p" "$1" missing.txt "$2"; printf "status=%s\n" "$?"' \
  sed-test "$sed_first" "$sed_last" 2>&1

echo "--- repeated standard input ---"
printf 'left\nright\n' | "$BIN" -c 'koshkit sed -n "1,3p" - -'

sed_order=$TEST_TEMP_DIRECTORY/koshkit-sed-order
printf 's/a/A/\n' > "$sed_order"

echo "--- interleaved script options preserve order ---"
printf 'a\n' | "$BIN" -c 'koshkit sed -f "$1" -e "s/A/B/"' \
  sed-test "$sed_order"

sed_left=$TEST_TEMP_DIRECTORY/koshkit-sed-left
sed_right=$TEST_TEMP_DIRECTORY/koshkit-sed-right
printf 's/a' > "$sed_left"
printf '/A/' > "$sed_right"

echo "--- adjacent script files share a boundary ---"
printf 'a\n' | "$BIN" -c 'koshkit sed -f "$1" -f "$2"' \
  sed-test "$sed_left" "$sed_right"

echo "--- empty explicit script ---"
printf 'unchanged\n' | "$BIN" -c "koshkit sed -e ''"

echo "--- a substitution reaches past a NUL byte ---"
printf 'a\0b\n' | "$BIN" -c 'koshkit sed s/b/X/' | "$BIN_DIR/invoke-koshkit" od -c

echo "--- a global substitution over a long line ---"
"$BIN" -c 'printf "%200000s" ""' | "$BIN_DIR/invoke-koshkit" tr ' ' a \
  | "$BIN" -c 'koshkit sed s/a/b/g' | "$BIN_DIR/invoke-koshkit" tr -d b | "$BIN_DIR/invoke-koshkit" wc -c

echo "--- quitting stops reading an endless input ---"
"$BIN" -c 'while echo y; do :; done 2> /dev/null | koshkit sed 3q'

echo "--- a file without a final newline is not joined to the next ---"
sed_directory=$(mktemp -d) || exit 1
trap '[ -n "$sed_directory" ] && "$BIN_DIR/invoke-koshkit" rm -rf -- "$sed_directory"' EXIT
printf 'last' > "$sed_directory/no-newline.txt"
printf 'next\n' > "$sed_directory/following.txt"
"$BIN" -c "koshkit sed '\$a\\appended' '$sed_directory/no-newline.txt' '$sed_directory/following.txt'"
