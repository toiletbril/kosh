set -e

tab=$(printf '\t')

result=$("$BIN" --debug-highlight-at 'keywords1=(if then case esac function); greet() { :; }; greet')
printf '%s\n' "$result" | grep -E "^keywords1${tab}assignment-name$"
printf '%s\n' "$result" | grep -E "^greet${tab}function-name$"
printf '%s\n' "$result" | grep -E "^greet${tab}resolved-command$"
if printf '%s\n' "$result" | grep -qE "${tab}keyword$"; then
  printf 'array elements were read as keywords\n'
fi

result=$("$BIN" --debug-highlight-at 'declare -a table=(case esac); echo ok')
printf '%s\n' "$result" | grep -E "^echo${tab}resolved-command$"
if printf '%s\n' "$result" | grep -qE "${tab}keyword$"; then
  printf 'a declare operand was read as keywords\n'
fi

result=$("$BIN" --debug-highlight-at 'arr=("$HOME/bin"); if true; then :; fi')
printf '%s\n' "$result" | grep -Fx "\$HOME${tab}variable"
printf '%s\n' "$result" | grep -E "^if${tab}keyword$"
printf '%s\n' "$result" | grep -E "^fi${tab}keyword$"
