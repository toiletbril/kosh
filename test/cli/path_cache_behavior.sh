#!/bin/sh

dir=$(mktemp -d)
trap '[ -n "$dir" ] && rm -rf "$dir"' EXIT
export TEST_NULL_OUTPUT=$dir/null-output

mkdir "$dir/one" "$dir/two" "$dir/blocked" "$dir/refresh" "$dir/appeared" \
    "$dir/completion-hot" "$dir/completion-late" "$dir/vanished" \
    "$dir/atomic" "$dir/atomic-replacement"
printf '#!/bin/sh\necho one\n' > "$dir/one/cacheprobe"
printf '#!/bin/sh\necho two\n' > "$dir/two/cacheprobe"
printf '#!/bin/sh\nexit 0\n' > "$dir/two/blockedprobe"
printf '#!/bin/sh\necho refreshed\n' > "$dir/staged"
printf '#!/bin/sh\necho appeared\n' > "$dir/staged-appeared"
printf '#!/bin/sh\necho late\n' > "$dir/staged-late"
chmod +x "$dir/one/cacheprobe" "$dir/two/cacheprobe"
chmod +x "$dir/two/blockedprobe" "$dir/staged" "$dir/staged-appeared" \
    "$dir/staged-late"
if [ "${OS-}" = Windows_NT ]; then
    mkdir "$dir/blocked/blockedprobe"
else
    : > "$dir/blocked/blockedprobe"
fi
printf '#!/bin/sh\n' > "$dir/vanished/vanishprobe"
chmod +x "$dir/vanished/vanishprobe"
printf '#!/bin/sh\n' > "$dir/atomic/oldatomicprobe"
printf '#!/bin/sh\n' > "$dir/atomic-replacement/newatomicprobe"
chmod +x "$dir/atomic/oldatomicprobe" \
    "$dir/atomic-replacement/newatomicprobe"

CACHE_SECOND="$dir/two" \
    PATH="$dir/one${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'for iteration in 1 2; do cacheprobe; PATH="$CACHE_SECOND$TEST_PATH_SEPARATOR$TEST_SYSTEM_PATH"; done'

PATH="$dir/blocked${TEST_PATH_SEPARATOR}$dir/two${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'blockedprobe > "$TEST_NULL_OUTPUT" 2>&1; printf "%s\n" "$?"'

PATH="$dir/blocked${TEST_PATH_SEPARATOR}$dir/two${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'compgen -c > "$TEST_NULL_OUTPUT" 2>&1
    blockedprobe > "$TEST_NULL_OUTPUT" 2>&1
    printf "%s\n" "$?"'

mkdir "$dir/warm-first" "$dir/warm-second"
printf '#!/bin/sh\necho stale-first\n' > "$dir/warm-first/warmprobe"
printf '#!/bin/sh\necho warm-second\n' > "$dir/warm-second/warmprobe"
chmod +x "$dir/warm-first/warmprobe" "$dir/warm-second/warmprobe"
WARM_FIRST="$dir/warm-first/warmprobe" WARM_REMOVED="$dir/warm-removed" \
    PATH="$dir/warm-first${TEST_PATH_SEPARATOR}$dir/warm-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'compgen -c warmprobe > "$TEST_NULL_OUTPUT" 2>&1
    koshkit mv "$WARM_FIRST" "$WARM_REMOVED"
    warmprobe'

mkdir "$dir/blocked-later"
if [ "${OS-}" = Windows_NT ]; then
    mkdir "$dir/blocked-later/recoverprobe"
else
    : > "$dir/blocked-later/recoverprobe"
fi
printf '#!/bin/sh\necho recovered\n' > "$dir/staged-recover"
chmod +x "$dir/staged-recover"
RECOVER_DIRECTORY="$dir/blocked-later" RECOVER_STAGED="$dir/staged-recover" \
    PATH="$dir/blocked-later${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'recoverprobe > "$TEST_NULL_OUTPUT" 2>&1
    koshkit mv "$RECOVER_DIRECTORY/recoverprobe" "$RECOVER_DIRECTORY/recoverprobe.removed"
    koshkit mv "$RECOVER_STAGED" "$RECOVER_DIRECTORY/recoverprobe"
    recoverprobe'

printf '#!/bin/sh\n' > "$dir/completion-hot/hotprobe"
chmod +x "$dir/completion-hot/hotprobe"
HOT_PROBE="$dir/completion-hot/hotprobe" HOT_REMOVED="$dir/hot-removed" \
    LATE_DIRECTORY="$dir/completion-late" LATE_STAGED="$dir/staged-late" \
    PATH="$dir/completion-hot${TEST_PATH_SEPARATOR}$dir/completion-late${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'compgen -c hotprobe 2>&-
    koshkit mv "$HOT_PROBE" "$HOT_REMOVED"
    removed=$(compgen -c hotprobe 2>&-)
    compgen -c lateprobe > "$TEST_NULL_OUTPUT" 2>&1 || :
    koshkit mv "$LATE_STAGED" "$LATE_DIRECTORY/lateprobe"
    added=$(compgen -c lateprobe 2>&-)
    printf "removed=%s added=%s\n" "$removed" "$added"'

VANISHED_DIRECTORY="$dir/vanished" \
    PATH="$dir/vanished${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'compgen -c vanishprobe > "$TEST_NULL_OUTPUT" 2>&1
    koshkit mv "$VANISHED_DIRECTORY" "$VANISHED_DIRECTORY.removed"
    vanished=$(compgen -c vanishprobe 2>&-)
    printf "vanished=%s\n" "$vanished"'

touch -r "$dir/atomic" "$dir/atomic-replacement"
ATOMIC_DIRECTORY="$dir/atomic" ATOMIC_REPLACEMENT="$dir/atomic-replacement" \
    PATH="$dir/atomic${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'compgen -c oldatomicprobe > "$TEST_NULL_OUTPUT" 2>&1
    koshkit mv "$ATOMIC_DIRECTORY" "$ATOMIC_DIRECTORY-old"
    koshkit mv "$ATOMIC_REPLACEMENT" "$ATOMIC_DIRECTORY"
    old=$(compgen -c oldatomicprobe 2>&-)
    new=$(compgen -c newatomicprobe 2>&-)
    printf "atomic-old=%s atomic-new=%s\n" "$old" "$new"'

CACHE_COMMAND=refreshed CACHE_DIRECTORY="$dir/refresh" \
    CACHE_STAGED="$dir/staged" \
    PATH="$dir/refresh${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'command -v "$CACHE_COMMAND" > "$TEST_NULL_OUTPUT" 2>&1
    koshkit mv "$CACHE_STAGED" "$CACHE_DIRECTORY/refreshed"
    hash -r
    "$CACHE_COMMAND"'

CACHE_COMMAND=appeared CACHE_DIRECTORY="$dir/appeared" \
    CACHE_STAGED="$dir/staged-appeared" \
    PATH="$dir/appeared${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'command -v "$CACHE_COMMAND" > "$TEST_NULL_OUTPUT" 2>&1
    koshkit mv "$CACHE_STAGED" "$CACHE_DIRECTORY/appeared"
    "$CACHE_COMMAND"'

mkdir "$dir/validation-first" "$dir/validation-second"
printf '#!/bin/sh\necho validation-first\n' > "$dir/validation-staged"
printf '#!/bin/sh\necho validation-second\n' > \
    "$dir/validation-second/validationprobe"
chmod +x "$dir/validation-staged" \
    "$dir/validation-second/validationprobe"
VALIDATION_FIRST="$dir/validation-first" \
    VALIDATION_STAGED="$dir/validation-staged" \
    PATH="$dir/validation-first${TEST_PATH_SEPARATOR}$dir/validation-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c '
    validationprobe
    koshkit mv "$VALIDATION_STAGED" "$VALIDATION_FIRST/validationprobe"
    compgen -c validationprobe > "$TEST_NULL_OUTPUT" 2>&1
    validationprobe
'

mkdir "$dir/query-first" "$dir/query-second"
printf '#!/bin/sh\necho compgen-first\n' > "$dir/compgen-staged"
printf '#!/bin/sh\necho compgen-second\n' > \
    "$dir/query-second/compgenprobe"
chmod +x "$dir/compgen-staged" "$dir/query-second/compgenprobe"
QUERY_FIRST="$dir/query-first" QUERY_STAGED="$dir/compgen-staged" \
    PATH="$dir/query-first${TEST_PATH_SEPARATOR}$dir/query-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'compgen -c compgenprobe > "$TEST_NULL_OUTPUT" 2>&1
    koshkit mv "$QUERY_STAGED" "$QUERY_FIRST/compgenprobe"
    compgenprobe'

printf '#!/bin/sh\necho command-first\n' > "$dir/command-staged"
printf '#!/bin/sh\necho command-second\n' > \
    "$dir/query-second/commandprobe"
chmod +x "$dir/command-staged" "$dir/query-second/commandprobe"
QUERY_FIRST="$dir/query-first" QUERY_STAGED="$dir/command-staged" \
    PATH="$dir/query-first${TEST_PATH_SEPARATOR}$dir/query-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'command -v commandprobe > "$TEST_NULL_OUTPUT"
    koshkit mv "$QUERY_STAGED" "$QUERY_FIRST/commandprobe"
    commandprobe'

printf '#!/bin/sh\necho type-first\n' > "$dir/type-staged"
printf '#!/bin/sh\necho type-second\n' > "$dir/query-second/typeprobe"
chmod +x "$dir/type-staged" "$dir/query-second/typeprobe"
QUERY_FIRST="$dir/query-first" QUERY_STAGED="$dir/type-staged" \
    PATH="$dir/query-first${TEST_PATH_SEPARATOR}$dir/query-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'type typeprobe > "$TEST_NULL_OUTPUT"
    koshkit mv "$QUERY_STAGED" "$QUERY_FIRST/typeprobe"
    typeprobe'

printf '#!/bin/sh\necho optimizer-first\n' > "$dir/optimizer-staged"
printf '#!/bin/sh\necho optimizer-second\n' > \
    "$dir/query-second/optimizerprobe"
chmod +x "$dir/optimizer-staged" "$dir/query-second/optimizerprobe"
QUERY_FIRST="$dir/query-first" QUERY_STAGED="$dir/optimizer-staged" \
    PATH="$dir/query-first${TEST_PATH_SEPARATOR}$dir/query-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'koshkit mv "$QUERY_STAGED" "$QUERY_FIRST/optimizerprobe"; optimizerprobe'

printf '#!/bin/sh\necho execution-first\n' > "$dir/execution-staged"
printf '#!/bin/sh\necho execution-second\n' > \
    "$dir/query-second/executionprobe"
chmod +x "$dir/execution-staged" "$dir/query-second/executionprobe"
QUERY_FIRST="$dir/query-first" QUERY_STAGED="$dir/execution-staged" \
    PATH="$dir/query-first${TEST_PATH_SEPARATOR}$dir/query-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'executionprobe; koshkit mv "$QUERY_STAGED" "$QUERY_FIRST/executionprobe"; executionprobe'

printf '#!/bin/sh\necho hash-first\n' > "$dir/hash-staged"
printf '#!/bin/sh\necho hash-second\n' > "$dir/query-second/hashprobe"
chmod +x "$dir/hash-staged" "$dir/query-second/hashprobe"
QUERY_FIRST="$dir/query-first" QUERY_STAGED="$dir/hash-staged" \
    PATH="$dir/query-first${TEST_PATH_SEPARATOR}$dir/query-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'hash hashprobe; koshkit mv "$QUERY_STAGED" "$QUERY_FIRST/hashprobe"; hashprobe'

printf '#!/bin/sh\necho mode-first\n' > "$dir/query-first/modeprobe"
printf '#!/bin/sh\necho mode-second\n' > "$dir/query-second/modeprobe"
chmod +x "$dir/query-second/modeprobe"
QUERY_FIRST="$dir/query-first" \
    PATH="$dir/query-first${TEST_PATH_SEPARATOR}$dir/query-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'compgen -c modeprobe > "$TEST_NULL_OUTPUT" 2>&1
    if [ "${OS-}" != Windows_NT ]; then
        /bin/chmod +x "$QUERY_FIRST/modeprobe"
    fi
    modeprobe'

mkdir "$dir/query-directory" "$dir/query-directory/directoryprobe"
: > "$dir/query-directory/regularprobe"
PATH="$dir/query-directory${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c '
    type -p directoryprobe > "$TEST_NULL_OUTPUT"
    printf "type-directory=%s " "$?"
    command -v directoryprobe > "$TEST_NULL_OUTPUT"
    printf "command-directory=%s " "$?"
    koshkit which directoryprobe > "$TEST_NULL_OUTPUT"
    printf "which-directory=%s\n" "$?"
    type -p regularprobe > "$TEST_NULL_OUTPUT"
    printf "type-regular=%s " "$?"
    command -v regularprobe > "$TEST_NULL_OUTPUT"
    printf "command-regular=%s\n" "$?"
'

analysis_log="$dir/analysis.log"
analysis_command=uname
analysis_path="$TEST_SYSTEM_PATH${TEST_PATH_SEPARATOR}$TEST_UNAME_DIRECTORY"
if [ "${OS-}" = Windows_NT ]; then
    mkdir "$dir/analysis"
    "$BIN" -c 'koshkit cp "$1" "$2"' \
        test-copy "$BIN" "$dir/analysis/analysis-probe.exe"
    analysis_command='analysis-probe -c true'
    analysis_path=$dir/analysis
fi
if "$BIN" -X all -c ':' >/dev/null 2>&1; then
    if ! PATH="$analysis_path" \
        "$BIN" --mood sh -WW -X all -c \
        "known_function() { :; }; alias known_alias=:; known_function; known_alias; $analysis_command > \"\$TEST_NULL_OUTPUT\"" \
        >/dev/null 2> "$analysis_log"
    then
        exit 1
    fi
    analysis_name=${analysis_command%% *}
    if grep -F "scanning PATH for 'known_function'" "$analysis_log" >/dev/null || \
        grep -F "scanning PATH for 'known_alias'" "$analysis_log" >/dev/null || \
        ! grep -F "scanning PATH for '$analysis_name'" "$analysis_log" >/dev/null
    then
        exit 1
    fi
fi
printf 'analysis-shadowed-no-path-scan\n'

noninteractive_log="$dir/noninteractive.log"
if "$BIN" -X all -c ':' >/dev/null 2>&1; then
    PATH="$dir/one${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
        "$BIN" -X all -c 'missing_command_xyz' \
        >/dev/null 2>"$noninteractive_log"
    if grep -F 'unique PATH directories to seed the program cache' \
        "$noninteractive_log" >/dev/null; then
        exit 1
    fi
fi
printf 'noninteractive-no-path-index\n'

path_update_log="$dir/path-update.log"
if "$BIN" -X all -c ':' >/dev/null 2>&1; then
    PATH="$dir/one${TEST_PATH_SEPARATOR}$dir/one${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH${TEST_PATH_SEPARATOR}$dir/one" "$BIN" -X info \
        --debug-complete-at cacheprobe >/dev/null 2>"$path_update_log" ||
        exit 1
    grep -F 'scanning 2 unique PATH directories' "$path_update_log" \
        >/dev/null || exit 1
fi
printf 'path-directories-deduplicated\n'

if "$BIN" -X all -c ':' >/dev/null 2>&1; then
    FIRST_DIRECTORY="$dir/one" \
    PATH="$dir/one${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -X info -c '
        compgen -c >/dev/null
        PATH="$PATH$TEST_PATH_SEPARATOR$FIRST_DIRECTORY"
        compgen -c >/dev/null
    ' >/dev/null 2>"$path_update_log" || exit 1
    [ "$(grep -c 'unique PATH directories' "$path_update_log")" -eq 1 ] ||
        exit 1
fi
printf 'duplicate-only-path-update-preserves-index\n'

PATH="$TEST_SYSTEM_PATH" "$BIN" -c \
    'compfunc() { :; }; eval "alias compalias=:"; compgen -c shopt 2>/dev/null; compgen -c compfunc 2>/dev/null; compgen -c compalias 2>/dev/null'

if [ "${OS-}" != Windows_NT ]; then
    mkdir "$dir/identity-real"
    /bin/ln -s "$dir/identity-real" "$dir/identity-path"
    IDENTITY_REAL="$dir/identity-real" \
    PATH="$dir/identity-path${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" \
        "$BIN" -c '
        compgen -c identity-new >/dev/null 2>&1
        compgen -f "$IDENTITY_REAL/identity-new" >/dev/null 2>&1
        printf "#!/bin/sh\n" > "$IDENTITY_REAL/identity-new"
        /bin/chmod +x "$IDENTITY_REAL/identity-new"
        file_result=$(compgen -f "$IDENTITY_REAL/identity-new" 2>/dev/null)
        [ "$file_result" = "$IDENTITY_REAL/identity-new" ] || exit 1
        printf "identity-alias=%s\n" \
            "$(compgen -c identity-new 2>/dev/null)"
    '

    mkdir "$dir/space path"
    : > "$dir/space path/file name"
    SPACED_DIRECTORY="$dir/space path" "$BIN" -c '
        result=$(compgen -f "$SPACED_DIRECTORY/file" 2>/dev/null)
        [ "$result" = "$SPACED_DIRECTORY/file name" ] && \
            echo compgen-file-literal-path
    '

    mkdir "$dir/mode-change"
    printf '#!/bin/sh\n' > "$dir/mode-change/mode-appeared"
    printf '#!/bin/sh\n' > "$dir/mode-change/mode-vanished"
    chmod +x "$dir/mode-change/mode-vanished"
    MODE_DIRECTORY="$dir/mode-change" \
    PATH="$dir/mode-change${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c '
        before=$(compgen -c mode-appeared 2>/dev/null)
        /bin/chmod +x "$MODE_DIRECTORY/mode-appeared"
        appeared=$(compgen -c mode-appeared 2>/dev/null)
        /bin/chmod -x "$MODE_DIRECTORY/mode-vanished"
        vanished=$(compgen -c mode-vanished 2>/dev/null)
        printf "mode-before=%s mode-appeared=%s mode-vanished=%s\n" \
            "$before" "$appeared" "$vanished"
    '

    mkdir "$dir/full-mode-change"
    printf '#!/bin/sh\n' > "$dir/full-mode-change/full-mode-probe"
    FULL_MODE_DIRECTORY="$dir/full-mode-change" \
        PATH="$dir/full-mode-change${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c '
        compgen -c >/dev/null 2>&1
        /bin/chmod +x "$FULL_MODE_DIRECTORY/full-mode-probe"
        compgen -c full-mode-probe 2>/dev/null
    '

    mkdir "$dir/broken-first" "$dir/broken-second"
    /bin/ln -s "$dir/missing" "$dir/broken-first/brokenprobe"
    printf '#!/bin/sh\necho unbroken\n' > "$dir/broken-second/brokenprobe"
    chmod +x "$dir/broken-second/brokenprobe"
    resolved=$(PATH="$dir/broken-first${TEST_PATH_SEPARATOR}$dir/broken-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
        'brokenprobe; compgen -c >/dev/null 2>&1; brokenprobe')
    if [ "$resolved" != "unbroken
unbroken" ]; then
        exit 1
    fi
else
    printf '%s\n' \
        identity-alias=identity-new \
        compgen-file-literal-path \
        'mode-before= mode-appeared=mode-appeared mode-vanished=' \
        full-mode-probe
fi
printf 'broken-symlink\n'

if [ "${OS-}" = Windows_NT ]; then
    mkdir "$dir/priority"
    : > "$dir/priority/priority.com"
    : > "$dir/priority/priority.exe"
    resolved=$(PATH="$dir/priority" "$BIN" -c \
        'compgen -c > "$TEST_NULL_OUTPUT"; command -v priority')
    case "$resolved" in
        *.exe) ;;
        *) exit 1 ;;
    esac

    resolved=$(PATH="$dir/priority" "$BIN" -c \
        'compgen -c PRIORITY 2>/dev/null')
    case "$resolved" in
        *priority*) ;;
        *) exit 1 ;;
    esac

    resolved=$(PATH="$dir/priority" "$BIN" -c \
        'command -v priority.com > "$TEST_NULL_OUTPUT"; command -v priority')
    case "$resolved" in
        *.exe) ;;
        *) exit 1 ;;
    esac

    mkdir "$dir/priority-first" "$dir/priority-second"
    : > "$dir/priority-first/cross.com"
    : > "$dir/priority-second/cross.exe"
    resolved=$(PATH="$dir/priority-first${TEST_PATH_SEPARATOR}$dir/priority-second" "$BIN" -c \
        'command -v cross.exe > "$TEST_NULL_OUTPUT"; command -v cross')
    case "$resolved" in
        *.com) ;;
        *) exit 1 ;;
    esac
else
    mkdir "$dir/suffix-isolation"
    printf '#!/bin/sh\n' > "$dir/suffix-isolation/suffix-only.exe"
    chmod +x "$dir/suffix-isolation/suffix-only.exe"
    if PATH="$dir/suffix-isolation" "$BIN" -c \
        'command -v suffix-only >/dev/null'
    then
        exit 1
    fi
fi
printf 'suffix-priority\n'

mkdir "$dir/source-first" "$dir/source-second"
printf 'echo sourced-non-runnable\n' > "$dir/source-first/sourceprobe"
printf '#!/bin/sh\necho wrong-source\n' > "$dir/source-second/sourceprobe"
chmod +x "$dir/source-second/sourceprobe"
PATH="$dir/source-first${TEST_PATH_SEPARATOR}$dir/source-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'source sourceprobe'

mkdir "$dir/all-first" "$dir/all-second"
printf '#!/bin/sh\n' > "$dir/all-first/allprobe"
printf '#!/bin/sh\n' > "$dir/all-second/allprobe"
chmod +x "$dir/all-first/allprobe" "$dir/all-second/allprobe"
PATH="$dir/all-first${TEST_PATH_SEPARATOR}$dir/all-first${TEST_PATH_SEPARATOR}$dir/all-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'type -a -p allprobe' | sed "s|$dir|DIR|g"

mkdir "$dir/hash-blocked"
mkdir "$dir/hash-blocked/hashblocked"
PATH="$dir/hash-blocked${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'hash hashblocked > "$TEST_NULL_OUTPUT" 2>&1
    printf "hash-blocked=%s\n" "$?"'

mkdir "$dir/source-directory-first" "$dir/source-file-second" \
    "$dir/source-directory-first/source-directory-probe"
printf 'echo source-skipped-directory\n' > \
    "$dir/source-file-second/source-directory-probe"
PATH="$dir/source-directory-first${TEST_PATH_SEPARATOR}$dir/source-file-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'source source-directory-probe'

mkdir "$dir/which-first" "$dir/which-second"
mkdir "$dir/which-first/whichblocked"
PATH="$dir/which-first${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'koshkit which whichblocked > "$TEST_NULL_OUTPUT" 2>&1
    printf "which-blocked=%s\n" "$?"'
printf '#!/bin/sh\necho which-first\n' > "$dir/which-staged"
printf '#!/bin/sh\necho which-second\n' > "$dir/which-second/whichprobe"
chmod +x "$dir/which-staged" "$dir/which-second/whichprobe"
WHICH_FIRST="$dir/which-first" WHICH_STAGED="$dir/which-staged" \
    PATH="$dir/which-first${TEST_PATH_SEPARATOR}$dir/which-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'whichprobe > "$TEST_NULL_OUTPUT"
    koshkit mv "$WHICH_STAGED" "$WHICH_FIRST/whichprobe"
    koshkit which whichprobe' | sed "s|$dir|DIR|g"

mkdir "$dir/command-p-first" "$dir/command-p-second"
printf '#!/bin/sh\necho command-p-first\n' > "$dir/command-p-staged"
printf '#!/bin/sh\necho command-p-second\n' > \
    "$dir/command-p-second/commandpprobe"
chmod +x "$dir/command-p-staged" "$dir/command-p-second/commandpprobe"
COMMAND_P_FIRST="$dir/command-p-first" \
    COMMAND_P_STAGED="$dir/command-p-staged" \
    PATH="$dir/command-p-first${TEST_PATH_SEPARATOR}$dir/command-p-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'commandpprobe
    command -p true
    koshkit mv "$COMMAND_P_STAGED" "$COMMAND_P_FIRST/commandpprobe"
    commandpprobe'

mkdir "$dir/equal-first" "$dir/equal-second"
printf '#!/bin/sh\necho equal-first\n' > "$dir/equal-staged"
printf '#!/bin/sh\necho equal-second\n' > "$dir/equal-second/equalprobe"
chmod +x "$dir/equal-staged" "$dir/equal-second/equalprobe"
EQUAL_FIRST="$dir/equal-first" EQUAL_STAGED="$dir/equal-staged" \
    PATH="$dir/equal-first${TEST_PATH_SEPARATOR}$dir/equal-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -c \
    'equalprobe
    # shellcheck disable=SC2269
    PATH="$PATH"
    koshkit mv "$EQUAL_STAGED" "$EQUAL_FIRST/equalprobe"
    equalprobe'

mkdir "$dir/mimic-first" "$dir/mimic-second"
printf '#!/bin/sh\necho mimic-first\n' > "$dir/mimic-staged"
printf '#!/bin/sh\necho mimic-second\n' > "$dir/mimic-second/mimicprobe"
chmod +x "$dir/mimic-staged" "$dir/mimic-second/mimicprobe"
printf '#!/bin/bash\nhash -r\nkoshkit mv "%s" "%s"\n' \
    "$dir/mimic-staged" "$dir/mimic-first/mimicprobe" > "$dir/mimic-script"
chmod +x "$dir/mimic-script"
MIMIC_SCRIPT="$dir/mimic-script" \
    PATH="$dir/mimic-first${TEST_PATH_SEPARATOR}$dir/mimic-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" -I --mood bash -c \
    'mimicprobe
    "$MIMIC_SCRIPT"
    mimicprobe'

mkdir "$dir/default-path"
printf '#!/bin/sh\n' > "$dir/default-path/onlycustom"
printf '#!/bin/sh\n' > "$dir/default-path/ls"
chmod +x "$dir/default-path/onlycustom" "$dir/default-path/ls"
if [ "${OS-}" = Windows_NT ]; then
    PATH="$dir/default-path" "$BIN" --mood bash -c \
        'command -p -v onlycustom > "$TEST_NULL_OUTPUT"
        printf "command-p-custom=%s\n" "$?"
        printf "DEFAULT-LS\n"'
else
    PATH="$dir/default-path" "$BIN" --mood bash -c \
        'command -p -v onlycustom >/dev/null
        printf "command-p-custom=%s\n" "$?"
        command -p -v ls' |
        sed "s|^/usr/bin/ls$|DEFAULT-LS|;s|^/bin/ls$|DEFAULT-LS|"
fi

"$BIN" --mood bash -c \
    'command -v missing-command-xyz echo printf
    printf "command-multiple=%s\n" "$?"'

mkdir "$dir/type-all-first" "$dir/type-all-second"
printf '#!/bin/sh\n' > "$dir/type-all-first/typeallprobe"
printf '#!/bin/sh\n' > "$dir/type-all-second/typeallprobe"
chmod +x "$dir/type-all-first/typeallprobe" \
    "$dir/type-all-second/typeallprobe"
PATH="$dir/type-all-first${TEST_PATH_SEPARATOR}$dir/type-all-second${TEST_PATH_SEPARATOR}$TEST_SYSTEM_PATH" "$BIN" --mood bash -c \
    'type -a -P typeallprobe' | sed "s|$dir|DIR|g"

if [ "${OS-}" != Windows_NT ]; then
    mkdir "$dir/slash-path"
    printf '#!/bin/sh\n' > "$dir/slash-path/slashprobe"
    chmod +x "$dir/slash-path/slashprobe"
    (
        cd "$dir/slash-path" || exit 1
        PATH=/bin "$BIN" --mood bash -c '
            type -p ./slashprobe
            type -P ./slashprobe
            koshkit which ./slashprobe
            hash ./slashprobe
            printf "hash-slash=%s\n" "$?"
            hash ./missing
            printf "hash-missing-slash=%s\n" "$?"
        '
    )
else
    printf '%s\n' \
        ./slashprobe \
        ./slashprobe \
        ./slashprobe \
        hash-slash=0 \
        hash-missing-slash=0
fi
