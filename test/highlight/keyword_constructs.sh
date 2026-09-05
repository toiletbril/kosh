set -e

tab=$(printf '\t')

"$BIN" --debug-highlight-at \
    'if true; then :; elif false; then :; else :; fi; while true; do :; done; for q in 1; do :; done' |
    grep -E "${tab}keyword$"

"$BIN" --debug-highlight-at \
    'case x in y) :;; esac; [[ -n x ]]; function f { :; }; time -p -R echo hi' |
    grep -E "${tab}(keyword|function-name|flag)$"

"$BIN" --debug-highlight-at 'then; done; fi; esac; do echo a; in foo' |
    grep -E "${tab}invalid-syntax$"

"$BIN" --debug-highlight-at \
    'echo "green" - -x -- --long=value --color="$PATH" *.kosh ./kosh-highlight-missing-path-xyz; kosh-highlight-missing-command-xyz' |
    grep -E "${tab}(string|flag|glob|invalid-path|unknown-command)$"

"$BIN" --debug-highlight-at 'if true
then echo "$MISSING"
fi' | grep -E "${tab}(keyword|unset-variable)$"

"$BIN" --debug-highlight-at 'for q
do :
done
for r
in a
do :
done' | grep -E "${tab}(keyword|variable)$"

"$BIN" --debug-highlight-at 'cat <<EOF
body
EOF
cat <<-"DONE"
	body
	DONE' | grep -E "${tab}(heredoc|heredoc-delimiter)$"

"$BIN" --debug-highlight-at 'cat <<\_ACEOF
body
_ACEOF
echo tail' | grep -E "${tab}(heredoc|heredoc-delimiter|resolved-command)$"

koshkit_result=$("$BIN" --debug-highlight-at \
    'koshkit ls -l; koshkit koshkit-missing-utility value; koshkit --help; koshkit "cat"; koshkit koshkit\-missing-escaped')
printf '%s\n' "$koshkit_result" | grep -Fx "ls${tab}resolved-command"
printf '%s\n' "$koshkit_result" |
    grep -Fx "koshkit-missing-utility${tab}unknown-command"
printf '%s\n' "$koshkit_result" | grep -Fx -- "--help${tab}flag"
printf '%s\n' "$koshkit_result" | grep -Fx '"cat"'"${tab}resolved-command"
printf '%s\n' "$koshkit_result" |
    grep -Fx "koshkit\\-missing-escaped${tab}unknown-command"
echo "koshkit static operands are classified"
