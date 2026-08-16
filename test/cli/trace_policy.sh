unset KOSH_FLAGS
d=$(mktemp -d)
trap '[ -n "$d" ] && rm -rf "$d"' EXIT

printf 'no_such_named_xyz\n' > "$d/direct"
out=$("$BIN" "$d/direct" 2>&1)
printf 'named traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$(printf 'no_such_stdin_xyz\n' | "$BIN" 2>&1)
printf 'stdin traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" -c "eval 'no_such_eval_xyz'" 2>&1)
printf 'eval traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" --no-diagnostics -c 'echo $(no_such_substitution_xyz)' 2>&1)
printf 'substitution traces=%s errors=%s parents=%s sites=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')" \
    "$(printf '%s\n' "$out" | grep -Fc 'echo $(no_such_substitution_xyz)')" \
    "$(printf '%s\n' "$out" | grep -c '^1:6: trace:')"

out=$("$BIN" --no-diagnostics \
    -c 'echo ${UNSET_TRACE_POLICY:-$(no_such_nested_substitution_xyz)}' 2>&1)
printf 'nested substitution traces=%s errors=%s parents=%s sites=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')" \
    "$(printf '%s\n' "$out" |
        grep -Fc 'echo ${UNSET_TRACE_POLICY:-$(no_such_nested_substitution_xyz)}')" \
    "$(printf '%s\n' "$out" | grep -c '^1:28: trace:')"

check_expansion_trace()
{
    label=$1
    site=$2
    source=$3
    out=$("$BIN" --no-diagnostics -c "$source" 2>&1)
    printf '%s traces=%s errors=%s sites=%s\n' \
        "$label" \
        "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
        "$(printf '%s\n' "$out" | grep -c 'error:')" \
        "$(printf '%s\n' "$out" | grep -c "^1:$site: trace:")"
}

check_expansion_trace pattern 23 \
    'value=x; echo ${value/$(bad_pattern_xyz)/x}'
check_expansion_trace replacement 25 \
    'value=x; echo ${value/x/$(bad_replacement_xyz)}'
check_expansion_trace case-modification 23 \
    'value=x; echo ${value^$(bad_case_xyz)}'
check_expansion_trace array-modifier 23 \
    'a[0]=x; echo ${a[0]/x/$(bad_array_modifier_xyz)}'
check_expansion_trace positional-modifier 22 \
    'set -- x; echo ${1/x/$(bad_positional_xyz)}'
check_expansion_trace arithmetic 15 \
    'echo $(( ${X:-$(bad_arithmetic_xyz)} ))'
check_expansion_trace substring 23 \
    'value=x; echo ${value:$(bad_substring_xyz)}'
check_expansion_trace subscript 18 \
    'a[0]=x; echo ${a[$(bad_subscript_xyz)]}'
check_expansion_trace indirect 43 \
    'target=; pointer=target; echo ${!pointer:-$(bad_indirect_xyz)}'
check_expansion_trace single-field 15 \
    'echo "${FAST:-$(bad_fast_xyz)}"'
check_expansion_trace positional-all 23 \
    'set -- x; echo "${@/x/$(bad_positional_all_xyz)}"'
check_expansion_trace array-all 23 \
    'a=(x); echo "${a[@]/x/$(bad_array_all_xyz)}"'
check_expansion_trace positional-default 20 \
    'set --; echo "${@:-$(bad_positional_default_xyz)}"'
check_expansion_trace array-default 21 \
    'a=(); echo "${a[@]:-$(bad_array_default_xyz)}"'
check_expansion_trace positional-slice 21 \
    'set -- x; echo "${@:$(bad_positional_slice_xyz)}"'
check_expansion_trace array-slice 21 \
    'a=(x); echo "${a[@]:$(bad_array_slice_xyz)}"'

out=$("$BIN" --no-diagnostics \
    -c 'echo ${ no_such_function_substitution_xyz; }' 2>&1)
printf 'function substitution traces=%s errors=%s parents=%s sites=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')" \
    "$(printf '%s\n' "$out" |
        grep -Fc 'echo ${ no_such_function_substitution_xyz; }')" \
    "$(printf '%s\n' "$out" | grep -c '^1:6: trace:')"

out=$("$BIN" --no-diagnostics \
    -c 'case X=$(no_such_case_copy_xyz) in *) :;; esac' 2>&1)
printf 'case assignment copy traces=%s errors=%s sites=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')" \
    "$(printf '%s\n' "$out" | grep -c '^1:8: trace:')"

out=$("$BIN" --no-diagnostics -c 'echo ${X:-$(if)}' 2>&1)
printf 'substitution parse traces=%s errors=%s sites=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')" \
    "$(printf '%s\n' "$out" | grep -c '^1:11: trace:')"

out=$("$BIN" --no-diagnostics -c 'echo $(echo ${X:-$(if)})' 2>&1)
printf 'nested parse traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" --no-diagnostics \
    -c 'koshkit cat <(no_such_process_substitution_xyz)' 2>&1)
case "$out" in
*error:*trace:*) process_order=error-first ;;
*) process_order=wrong ;;
esac
printf 'process substitution traces=%s errors=%s sites=%s order=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')" \
    "$(printf '%s\n' "$out" | grep -c '^1:13: trace:')" \
    "$process_order"

out=$("$BIN" --no-diagnostics \
    -c 'koshkit cat <(eval no_such_process_eval_xyz)' 2>&1)
printf 'process eval traces=%s errors=%s inner-sites=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')" \
    "$(printf '%s\n' "$out" | grep -c '^1:1: trace:')"

out=$("$BIN" --no-diagnostics \
    -c 'koshkit cat <(echo prefix >&2; eval no_such_prefixed_process_eval_xyz)' \
    2>&1)
printf 'prefixed process eval traces=%s errors=%s prefixes=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')" \
    "$(printf '%s\n' "$out" | grep -c '^prefix$')"

out=$("$BIN" --no-traces --no-diagnostics \
    -c 'echo $(no_such_suppressed_substitution_xyz)' 2>&1)
printf 'disabled-substitution traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" --no-traces --no-diagnostics \
    -c 'koshkit cat <(eval no_such_suppressed_process_substitution_xyz)' 2>&1)
printf 'disabled-process-substitution traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" -c no_such_first_root_xyz -c no_such_second_root_xyz 2>&1)
printf 'multi-root traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"
printf 'multi-root first=%s second=%s\n' \
    "$(printf '%s\n' "$out" | grep -c 'no_such_first_root_xyz')" \
    "$(printf '%s\n' "$out" | grep -c 'no_such_second_root_xyz')"

out=$("$BIN" -n -WW \
    -c 'echo "$TRACE_FIRST"; TRACE_FIRST=x; echo "$TRACE_SECOND"; TRACE_SECOND=x' \
    -c : 2>&1)
printf 'noexec-batch traces=%s warnings=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'warning:')"

recursive='if ((depth)); then let depth-=1; eval "$recursive"; else no_such_recursive_xyz; fi'
out=$("$BIN" -c 'recursive=$1; depth=5; eval "$recursive"' trace-driver "$recursive" 2>&1)
printf 'recursive traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" --mood bash -c 'eval "$1"' root 'eval "$2"' '?????????' 2>&1)
warning_block=$(printf '%s\n' "$out" | sed '/error:/,$d')
printf 'runtime-repeat traces=%s warnings=%s\n' \
    "$(printf '%s\n' "$warning_block" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$warning_block" | grep -c 'warning:')"

printf '%s\n' 'current=$NEXT' 'NEXT=$AFTER' '. "$current"' > "$d/identity-a"
printf '%s\n' 'current=$NEXT' 'NEXT=$AFTER' '. "$current"' > "$d/identity-b"
printf 'no_such_identity_xyz\n' > "$d/identity-c"
out=$(NEXT="$d/identity-b" AFTER="$d/identity-c" \
    "$BIN" -c '. "$1"' trace-driver "$d/identity-a" 2>&1)
printf 'identity traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" --no-traces -c "eval 'no_such_suppressed_xyz'" 2>&1)
printf 'disabled-runtime traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" --no-traces -c no_such_first_root_xyz -c no_such_second_root_xyz 2>&1)
printf 'disabled-multi-root traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

printf '[[ x = "$UNSET_TRACE_POLICY" ]]\n' > "$d/warning"
out=$("$BIN" -W --no-traces -c '. "$1"' trace-driver "$d/warning" 2>&1)
printf 'disabled-warning traces=%s warnings=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'warning:')"

out=$("$BIN" --no-traces -c "eval '{'" 2>&1)
printf 'disabled-parse traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

printf "eval 'no_such_fallback_xyz'\n" > "$d/fallback"
chmod +x "$d/fallback"
out=$("$BIN" -c '"$1"' trace-driver "$d/fallback" 2>&1)
printf 'enabled-fallback traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

printf '{\n' > "$d/fallback-parse"
chmod +x "$d/fallback-parse"
out=$("$BIN" -c '"$1"' trace-driver "$d/fallback-parse" 2>&1)
printf 'enabled-fallback-parse traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

out=$("$BIN" --no-traces -c '"$1"' trace-driver "$d/fallback" 2>&1)
printf 'disabled-fallback traces=%s errors=%s\n' \
    "$(printf '%s\n' "$out" | grep -Ec 'trace:')" \
    "$(printf '%s\n' "$out" | grep -c 'error:')"

unset KOSH_FLAGS
out=$("$BIN" -c 'echo hi' -c no_such_command_xyz \
    '$()' ';' '*' '#' '~' '\' '"' 2>&1)
rc=$?
trace_line=$(printf '%s\n' "$out" | grep 'trace:' -A1 | tail -1)
trace_line="     1 |  KOSH -c ${trace_line#* -c }"
printf '%s\n' "$trace_line"
printf '%s\n' "$out" | grep -q -- "-c 'echo hi' -c no_such_command_xyz" && echo "quoted_and_second_c=ok"
printf '%s\n' "$out" |
  grep -Fq "'\$()' ';' '*' '#' '~' '\\' '\"'" &&
  echo "special_arguments_quoted=ok"
echo "rc=$rc"

unset KOSH_FLAGS
out=$("$BIN" -c no_such_command_xyz 2>&1)
rc=$?
printf 'traces=%s\n' "$(printf '%s\n' "$out" | grep -Ec 'trace:')"
printf '%s\n' "$out" | grep -q 'no_such_command_xyz' && echo "error=ok"
echo "rc=$rc"

unset KOSH_FLAGS
# A missing script file operand with spaces in its name is wrapped in single
# quotes in the trace line, and the caret lands on the quoted operand.
out=$("$BIN" "no such script file" 2>&1)
rc=$?
printf '%s\n' "$out" | grep -o "Could not open 'no such script file'"
printf '%s\n' "$out" | grep -o "'no such script file'"
echo "rc=$rc"
