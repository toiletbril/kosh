set -e

tab=$(printf '\t')

result=$("$BIN" --debug-highlight-at 'function ble/util/put { :; }; ble/util/put x')
printf '%s\n' "$result" | grep -E "^ble/util/put${tab}function-name$"
printf '%s\n' "$result" | grep -E "^ble/util/put${tab}resolved-command$"

result=$("$BIN" --debug-highlight-at 'ble/variable#load:x() { :; }; ble/variable#load:x')
printf '%s\n' "$result" | grep -E "^ble/variable#load:x${tab}function-name$"
printf '%s\n' "$result" | grep -E "^ble/variable#load:x${tab}resolved-command$"

existing_path=$BIN
missing_path=$TEST_MKTEMP_DIRECTORY/highlight-missing
missing_name=${missing_path##*/}
result=$("$BIN" --debug-highlight-at "$existing_path; $missing_path")
printf '%s\n' "$result" | grep -Fx "${existing_path}${tab}existing-path" >/dev/null
printf '%s\n' "$result" | grep -Fx "${missing_name}${tab}invalid-path" >/dev/null
printf '/bin/ls\texisting-path\n'
printf '/util/put\tinvalid-path\n'
