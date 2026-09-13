# A missing required argument reports its location and points to the help.
# These fixed error cases do not send signals or modify files.
unset KOSH_FLAGS

echo "--- builtin getopts with no arguments ---"
"$BIN" -c 'getopts' 2>&1; echo "rc=$?"
echo "--- builtin let with no expression ---"
{
    "$BIN" -c 'let' 2>&1
    printf 'rc=%s\n' "$?"
} | ./normalize-trace.sh "$BIN"
echo "--- calc with no expression ---"
"$BIN" -c 'koshkit calc' 2>&1; echo "rc=$?"
echo "--- koshkit cp with one operand ---"
"$BIN" -c 'koshkit cp onlyone' 2>&1; echo "rc=$?"
echo "--- koshkit grep with no pattern ---"
"$BIN" -c 'koshkit grep' 2>&1; echo "rc=$?"
for utility in goodcore goodfsw goodnode goodstat retry stat watch; do
  echo "--- koshkit $utility with no required argument ---"
  "$BIN" -c "koshkit $utility" 2>&1; echo "rc=$?"
done
echo "--- goodcore locates an invalid process id ---"
"$BIN" -c 'koshkit goodcore --pid nope' 2>&1; echo "rc=$?"
echo "--- goodcore locates conflicting flags ---"
"$BIN" -c 'koshkit goodcore --pid 1 --binary program' 2>&1; echo "rc=$?"
echo "--- goodcore locates conflicting input ---"
"$BIN" -c 'koshkit goodcore --pid 1 core' 2>&1; echo "rc=$?"
echo "--- goodcore locates an extra core operand ---"
"$BIN" -c 'koshkit goodcore first second' 2>&1; echo "rc=$?"
echo "--- goodnode locates an invalid inode ---"
"$BIN" -c 'koshkit goodnode --inode nope' 2>&1; echo "rc=$?"
echo "--- goodnode locates an invalid color mode ---"
"$BIN" -c 'koshkit --color rainbow goodnode .' 2>&1; echo "rc=$?"
echo "--- koshkit sync data mode with no file ---"
"$BIN" -c 'koshkit sync -d' 2>&1; echo "rc=$?"
echo "--- koshkit sync filesystem mode with no file ---"
"$BIN" -c 'koshkit sync -f' 2>&1; echo "rc=$?"
echo "--- the bash mood locates the note ---"
"$BIN" --mood bash -c 'koshkit mkdir' 2>&1; echo "rc=$?"
echo "--- the sh mood locates the note ---"
"$BIN" --mood sh -c 'koshkit mkdir' 2>&1; echo "rc=$?"
