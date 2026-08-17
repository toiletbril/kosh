set -e

tab=$(printf '\t')

result=$("$BIN" --debug-highlight-at 'function ble/util/put { :; }; ble/util/put x')
printf '%s\n' "$result" | grep -E "^ble/util/put${tab}function-name$"
printf '%s\n' "$result" | grep -E "^ble/util/put${tab}resolved-command$"

result=$("$BIN" --debug-highlight-at 'ble/variable#load:x() { :; }; ble/variable#load:x')
printf '%s\n' "$result" | grep -E "^ble/variable#load:x${tab}function-name$"
printf '%s\n' "$result" | grep -E "^ble/variable#load:x${tab}resolved-command$"

result=$("$BIN" --debug-highlight-at '/bin/ls; ble/util/put y')
printf '%s\n' "$result" | grep -E "^/bin/ls${tab}existing-path$"
printf '%s\n' "$result" | grep -E "^/util/put${tab}invalid-path$"
