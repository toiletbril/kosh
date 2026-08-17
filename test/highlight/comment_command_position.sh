set -e

tab=$(printf '\t')

result=$("$BIN" --debug-highlight-at 'echo one
# a note
echo two')
printf '%s\n' "$result" | grep -Fx "# a note${tab}comment"
printf '%s\n' "$result" | grep -c -Fx "echo${tab}resolved-command"

result=$("$BIN" --debug-highlight-at 'function outer {
  # a note
  echo body
}
outer')
printf '%s\n' "$result" | grep -Fx "echo${tab}resolved-command"
printf '%s\n' "$result" | grep -Fx "outer${tab}resolved-command"
