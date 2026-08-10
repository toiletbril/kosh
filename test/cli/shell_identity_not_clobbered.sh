unset KOSH_FLAGS
# SHELL is owned by login, getty, or the display manager, so kosh leaves an
# inherited value untouched the way bash never reassigns it, and seeds its own
# invocation path only when SHELL is unset. BASH names the invocation path on
# its own, the symlink spelling such as a bash symlink to kosh. The output is
# filtered to the markers so a sourced profile cannot perturb it.
echo "== an inherited SHELL is preserved, not clobbered:"
inherited_shell=$PWD/sentinel-shell
if [ "${OS-}" = Windows_NT ]; then
    inherited_shell=$(pwd -W)/sentinel-shell
fi
SHELL="$inherited_shell" EXPECTED_SHELL="$inherited_shell" "$BIN" -c '
    normalized_shell=$(printf "%s\n" "$SHELL" | koshkit tr "\\" "/")
    normalized_expected_shell=$(printf "%s\n" "$EXPECTED_SHELL" | koshkit tr "\\" "/")
    if [ "$normalized_shell" = "$normalized_expected_shell" ]; then echo preserved; else echo "clobbered to $SHELL"; fi
'

echo "== an unset SHELL is seeded with the invocation path:"
(unset SHELL
 "$BIN" -c 'if [ "$SHELL" = "$0" ]; then echo seeded-invocation; else echo "seeded $SHELL not $0"; fi')

dir=$(mktemp -d)
if [ "${OS-}" = Windows_NT ]; then
    "$BIN" -c 'koshkit cp "$1" "$2"' test-copy "$BIN" "$dir/bash.exe"
    bash_invocation=$dir/bash.exe
    bash_pattern='*bash.exe'
else
    ln -s "$BIN" "$dir/bash"
    bash_invocation=$dir/bash
    bash_pattern='*/bash'
fi
echo "== BASH names the invocation symlink while SHELL stays inherited:"
SHELL="$inherited_shell" EXPECTED_SHELL="$inherited_shell" \
    BASH_PATTERN="$bash_pattern" "$bash_invocation" -c \
    'case "$BASH" in $BASH_PATTERN) echo bash-is-invocation ;; *) echo "BASH=$BASH" ;; esac
     normalized_shell=$(printf "%s\n" "$SHELL" | koshkit tr "\\" "/")
     normalized_expected_shell=$(printf "%s\n" "$EXPECTED_SHELL" | koshkit tr "\\" "/")
     if [ "$normalized_shell" = "$normalized_expected_shell" ]; then echo shell-preserved; else echo "SHELL=$SHELL"; fi' \
    2>/dev/null
rm -rf "$dir"
