set -e

tab=$(printf '\t')

result=$("$BIN" --debug-highlight-at 'mapfile -t v <<< "$HOME"
echo after')
printf '%s\n' "$result" | grep -Fx "<<<${tab}operator"
printf '%s\n' "$result" | grep -E "^echo${tab}resolved-command$"
if printf '%s\n' "$result" | grep -qE "${tab}heredoc(-delimiter)?$"; then
  printf 'a here-string opened a here-document\n'
fi

result=$("$BIN" --debug-highlight-at '((w<<=1, w>2 && (w=2)))
echo after')
printf '%s\n' "$result" | grep -E "^echo${tab}resolved-command$"
if printf '%s\n' "$result" | grep -qE "${tab}heredoc(-delimiter)?$"; then
  printf 'an arithmetic shift opened a here-document\n'
fi

result=$("$BIN" --debug-highlight-at 'cat <<EOF
body
EOF
echo after')
printf '%s\n' "$result" | grep -Fx "EOF${tab}heredoc-delimiter"
printf '%s\n' "$result" | grep -E "${tab}heredoc$"
printf '%s\n' "$result" | grep -E "^echo${tab}resolved-command$"
