unset KOSH_FLAGS
"$BIN" -c 'echo one' -c 'echo two'
"$BIN" --command='echo equals-form'
"$BIN" -E -c 'echo trailer'
echo "rc=$?"

success_output=$("$BIN" --no-diagnostics -E -c ':' 2>&1)
[ -z "$success_output" ] || exit 1
echo "show-exit-code success is quiet"

continued_output=$("$BIN" --no-diagnostics -E -c 'false; echo after' 2>&1)
[ "$?" -eq 0 ] || exit 1
[ "$(printf '%s\n' "$continued_output" | grep -c 'error: nonzero exit code: 1')" -eq 1 ] || exit 1
printf '%s\n' "$continued_output" | grep -Eq '^[0-9]+:[0-9]+: error: nonzero exit code: 1\.$' || exit 1
echo "show-exit-code reports a located continued failure"

errexit_output=$("$BIN" --no-diagnostics -Ee -c 'false; echo never' 2>&1)
errexit_status=$?
[ "$errexit_status" -eq 1 ] || exit 1
[ "$(printf '%s\n' "$errexit_output" | grep -c 'error: nonzero exit code: 1')" -eq 1 ] || exit 1
echo "show-exit-code reports before errexit"

guarded_output=$("$BIN" --no-diagnostics -E -c 'false || true; ! false; if false; then :; fi; echo after' 2>&1)
[ "$?" -eq 0 ] || exit 1
[ "$(printf '%s\n' "$guarded_output" | grep -c 'error: nonzero exit code: 1')" -eq 2 ] || exit 1
echo "show-exit-code reports guarded failures once"

pipeline_output=$("$BIN" --no-diagnostics -E -c 'set +o pipefail; false | true; echo after' 2>&1)
[ "$?" -eq 0 ] || exit 1
[ "$(printf '%s\n' "$pipeline_output" | grep -c 'error: nonzero exit code:')" -eq 0 ] || exit 1
pipefail_output=$("$BIN" --no-diagnostics -E -c 'set -o pipefail; false | true; echo after' 2>&1)
[ "$?" -eq 0 ] || exit 1
[ "$(printf '%s\n' "$pipefail_output" | grep -c 'error: nonzero exit code: 1')" -eq 1 ] || exit 1
echo "show-exit-code follows the effective pipeline status"
