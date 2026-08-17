set -e

tab=$(printf '\t')

result=$("$BIN" --debug-highlight-at 'function ble/highlight/layer:{selection}/getg { :; }; ble/highlight/layer:{selection}/getg')
printf '%s\n' "$result" | grep -Fx "ble/highlight/layer:{selection}/getg${tab}function-name"
printf '%s\n' "$result" | grep -Fx "ble/highlight/layer:{selection}/getg${tab}resolved-command"

result=$("$BIN" --debug-highlight-at 'ble/prompt/unit:{section}/get() { :; }; ble/prompt/unit:{section}/get')
printf '%s\n' "$result" | grep -Fx "ble/prompt/unit:{section}/get${tab}function-name"
printf '%s\n' "$result" | grep -Fx "ble/prompt/unit:{section}/get${tab}resolved-command"

result=$("$BIN" --debug-highlight-at 'function bad{1..3} { :; }')
printf '%s\n' "$result" | grep -Fx "bad{1..3}${tab}invalid-syntax"

result=$("$BIN" --debug-highlight-at 'function bad{unclosed { :; }')
printf '%s\n' "$result" | grep -Fx "bad{unclosed${tab}invalid-syntax"
