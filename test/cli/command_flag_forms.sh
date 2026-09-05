unset KOSH_FLAGS
"$BIN" -c 'echo one' -c 'echo two'
"$BIN" --command='echo equals-form'
"$BIN" -E -c 'echo trailer'
echo "rc=$?"
"$BIN" --no-init-files -c ''
echo "empty-command-rc=$?"

success_output=$("$BIN" --no-diagnostics -E -c ':' 2>&1)
[ -z "$success_output" ] || exit 1
echo "show-exit-code success is quiet"

"$BIN" --help 2>&1 | grep -F 'Show diagnostics for every non-zero exit code.' >/dev/null || exit 1
"$BIN" -c 'set --help' 2>&1 | grep -F 'Show diagnostics for every non-zero exit code.' >/dev/null || exit 1
echo "show-exit-code help describes diagnostics"

continued_output=$("$BIN" --no-diagnostics -E -c 'false; echo after' 2>&1)
[ "$?" -eq 0 ] || exit 1
[ "$(printf '%s\n' "$continued_output" | grep -c 'warning: Non-zero exit code (1)')" -eq 1 ] || exit 1
printf '%s\n' "$continued_output" | grep -Eq '^[0-9]+:[0-9]+: warning: Non-zero exit code \(1\)$' || exit 1
echo "show-exit-code reports a located continued failure"

errexit_output=$("$BIN" --no-diagnostics -Ee -c 'false; echo never' 2>&1)
errexit_status=$?
[ "$errexit_status" -eq 1 ] || exit 1
[ "$(printf '%s\n' "$errexit_output" | grep -c 'error: Non-zero exit code (1)')" -eq 1 ] || exit 1
printf '%s\n' "$errexit_output" | grep -Eq '^[0-9]+:[0-9]+: error: Non-zero exit code \(1\)$' || exit 1
echo "show-exit-code reports before errexit"

guarded_output=$("$BIN" --no-diagnostics -Ee -c 'false || true; ! false; if false; then :; fi; echo after' 2>&1)
[ "$?" -eq 0 ] || exit 1
[ "$(printf '%s\n' "$guarded_output" | grep -c 'warning: Non-zero exit code (1)')" -eq 2 ] || exit 1
echo "show-exit-code reports guarded failures once"

pipeline_output=$("$BIN" --no-diagnostics -E -c 'set +o pipefail; false | true; echo after' 2>&1)
[ "$?" -eq 0 ] || exit 1
[ "$(printf '%s\n' "$pipeline_output" | grep -c 'Non-zero exit code')" -eq 0 ] || exit 1
pipefail_output=$("$BIN" --no-diagnostics -E -c 'set -o pipefail; false | true; echo after' 2>&1)
[ "$?" -eq 0 ] || exit 1
[ "$(printf '%s\n' "$pipefail_output" | grep -c 'warning: Non-zero exit code (1)')" -eq 1 ] || exit 1
echo "show-exit-code follows the effective pipeline status"

pipefail_errexit_output=$("$BIN" --no-diagnostics -Ee -c 'set -o pipefail; false | true; echo never' 2>&1)
[ "$?" -eq 1 ] || exit 1
[ "$(printf '%s\n' "$pipefail_errexit_output" | grep -c 'error: Non-zero exit code (1)')" -eq 1 ] || exit 1
echo "show-exit-code reports fatal pipefail as an error"

runtime_option_output=$("$BIN" --no-diagnostics -c 'set -o show-exit-code; "$1" --no-diagnostics -c "exit 7"' kosh-test "$BIN" 2>&1)
[ "$?" -eq 7 ] || exit 1
[ "$(printf '%s\n' "$runtime_option_output" | grep -c 'warning: Non-zero exit code (7)')" -eq 1 ] || exit 1
echo "runtime show-exit-code survives the terminal command"

runtime_errexit_output=$(
  "$BIN" --no-diagnostics -c 'set -o show-exit-code; set -e; false' 2>&1
)
runtime_errexit_status=$?
[ "$runtime_errexit_status" -eq 1 ] || exit 1
[ "$(printf '%s\n' "$runtime_errexit_output" | grep -c 'error: Non-zero exit code (1)')" -eq 1 ] || exit 1
echo "runtime show-exit-code reports errexit as an error"

runtime_stats_output=$("$BIN" --no-diagnostics -c 'set -o show-stats; "$1" --no-diagnostics -c ":"' kosh-test "$BIN" 2>&1)
[ "$?" -eq 0 ] || exit 1
printf '%s\n' "$runtime_stats_output" | grep -F '[Stats' >/dev/null || exit 1
runtime_memory_output=$("$BIN" --no-diagnostics -c 'set -o show-memory; "$1" --no-diagnostics -c ":"' kosh-test "$BIN" 2>&1)
[ "$?" -eq 0 ] || exit 1
printf '%s\n' "$runtime_memory_output" | grep -F 'AST arena:' >/dev/null || exit 1
echo "runtime reports survive the terminal command"
