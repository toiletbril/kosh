# A builtin or a utility that is missing a required argument renders a located
# error followed by a note that points at the help, the same caret in every
# mood. Each case is a fixed error path that delivers no real signal and touches
# no file.
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
echo "--- the note is located in the bash mood too ---"
"$BIN" --mood bash -c 'koshkit mkdir' 2>&1; echo "rc=$?"
echo "--- and in the sh mood ---"
"$BIN" --mood sh -c 'koshkit mkdir' 2>&1; echo "rc=$?"
