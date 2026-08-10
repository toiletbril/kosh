#!/bin/bash
# A configure-style script that stresses the bash surface of the evaluator with
# arrays, parameter expansion, arithmetic, conditionals, case fall-through,
# regular expressions, printf, and brace expansion. The heavy loops spawn no
# external command, so the run time measures evaluation speed rather than
# process creation. The output is deterministic, so the same input yields the
# same bytes on bash and on kosh, which lets a run double as a correctness
# check. The work scales by SCALE the way the POSIX configure.sh does, with the
# dense quadratic and per-character loops carrying the bulk of the cost.

set -u

# Extended globs are enabled before any function is defined, since bash parses
# an extglob pattern in a case label when it reads the function, not when it
# runs it.
shopt -s extglob

# An optimizing shell's default mood arrives with failglob strict, which a
# sparse array literal like ([2]=a b) trips, so the script relaxes it the way
# a configure run negotiates any feature. bash spells failglob as a shopt, so
# its set rejects the name quietly here and the run continues unchanged.
set +o failglob 2>/dev/null || true

# The work multiplier. A larger value lengthens every loop.
SCALE=${SCALE:-40}

PASS_COUNT=0
FAIL_COUNT=0
WORK_UNITS=0

# Build-style feature toggles, assigned once and never reassigned, the shape a
# real configure run carries. An optimizing shell can prove every guard that
# reads them dead at parse time, while bash tests each guard on every pass
# through the hot loops.
CONFIG_TRACE=0
CONFIG_PROFILE=0
CONFIG_PARANOID=0
CONFIG_RELEASE=1

# Record a probe result, comparing an actual value to an expected one.
check() {
    local label="$1" actual="$2" expected="$3"
    if [[ $actual == "$expected" ]]; then
        PASS_COUNT=$((PASS_COUNT + 1))
        printf 'checking %s... %s\n' "$label" "$actual"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        printf 'checking %s... MISMATCH got %s want %s\n' "$label" "$actual" "$expected"
    fi
}

# Announce a stage before its work runs, so the output streams through the run
# the way a real configure prints, rather than arriving in one burst at the
# end of each probe.
announce() {
    printf 'configuring %s\n' "$1"
}

# ----------------------------------------------------------------------------
# A coprime field, the heavy quadratic core. For every pair (i, j) up to SCALE
# it computes the greatest common divisor through the euclidean loop, counts the
# coprime pairs, and folds a rolling checksum, all in arithmetic and arrays.
# ----------------------------------------------------------------------------
probe_coprime_field() {
    announce "the coprime field"
    local i j a b t coprime=0 checksum=0
    local -a column=()
    for ((i = 1; i <= SCALE; i++)); do
        for ((j = 1; j <= SCALE; j++)); do
            a=$i
            b=$j
            while ((b != 0)); do
                t=$((a % b))
                a=$b
                b=$t
            done
            if ((a == 1)); then
                coprime=$((coprime + 1))
            fi
            # Dead diagnostics in the quadratic core, a constant guard and a
            # literal test, both provable at parse time and both re-tested
            # per pair by bash.
            if [ "$CONFIG_TRACE" -eq 1 ]; then
                printf 'trace: pair %d %d\n' "$i" "$j"
            fi
            if [ 0 -eq 1 ]; then
                coprime=$((coprime * 2))
            fi
            checksum=$(((checksum + a * j + i) % 1000003))
            WORK_UNITS=$((WORK_UNITS + 1))
        done
        column[i]=$checksum
    done
    check "coprime field nonempty" "$((coprime > 0 ? 1 : 0))" "1"
    check "coprime checksum stable" "$((checksum == column[SCALE] ? 1 : 0))" "1"
    check "coprime column length" "${#column[@]}" "$SCALE"
}

# ----------------------------------------------------------------------------
# A string forge, the per-character heavy loop. Each round reverses a word,
# folds an ordinal checksum, and runs a chain of parameter expansions over a
# pool of words, exercising removal, replacement, case modification, the tilde
# toggle, substring, and length.
# ----------------------------------------------------------------------------
ordinal_sum() {
    local s="$1" i ch acc=0
    for ((i = 0; i < ${#s}; i++)); do
        ch=${s:i:1}
        case $ch in
        [a-z]) acc=$((acc + 97)) ;;
        [A-Z]) acc=$((acc + 65)) ;;
        [0-9]) acc=$((acc + 48)) ;;
        *) acc=$((acc + 32)) ;;
        esac
    done
    echo "$acc"
}

probe_string_forge() {
    announce "the string forge"
    local -a pool=(configure-script alpha_beta_gamma BashTortureCase token123list
        Mixed-Case-Words under_score_name HELLO-world snake_AND_kebab)
    local round word rev i ch checksum=0 expansions=0
    for ((round = 0; round < SCALE; round++)); do
        for word in "${pool[@]}"; do
            # Reverse the word one character at a time.
            rev=""
            for ((i = 0; i < ${#word}; i++)); do
                ch=${word:i:1}
                rev=$ch$rev
            done
            # A chain of parameter expansions over the word.
            local head=${word%%[-_]*}
            local tail=${word##*[-_]}
            local under=${word//-/_}
            local upper=${word^^}
            local lower=${word,,}
            local toggled=${word~}
            local sub=${word:2:4}
            checksum=$((checksum + ${#rev} + ${#head} + ${#tail} + ${#under} +
                ${#upper} + ${#lower} + ${#toggled} + ${#sub}))
            expansions=$((expansions + 8))
            WORK_UNITS=$((WORK_UNITS + 1))
        done
    done
    check "forge checksum positive" "$((checksum > 0 ? 1 : 0))" "1"
    check "forge expansion count" "$expansions" "$((SCALE * 64))"
    check "ordinal helper" "$(ordinal_sum abc)" "291"
}

# ----------------------------------------------------------------------------
# A regex scanner, matching version-like tags against an extended regular
# expression and folding the captured groups from BASH_REMATCH.
# ----------------------------------------------------------------------------
probe_regex_scan() {
    announce "the regex scanner"
    local re='^v([0-9]+)\.([0-9]+)(-([a-z]+))?$'
    local -a tags=(v1.0 v2.5-beta v10.3 v0.9-rc nope v7.7-final)
    local round tag total=0 named=0
    for ((round = 0; round < SCALE; round++)); do
        for tag in "${tags[@]}"; do
            if [[ $tag =~ $re ]]; then
                total=$((total + BASH_REMATCH[1] * 100 + BASH_REMATCH[2]))
                if [[ -n ${BASH_REMATCH[4]} ]]; then
                    named=$((named + ${#BASH_REMATCH[4]}))
                fi
            fi
            WORK_UNITS=$((WORK_UNITS + 1))
        done
    done
    check "regex total positive" "$((total > 0 ? 1 : 0))" "1"
    check "regex named positive" "$((named > 0 ? 1 : 0))" "1"

    [[ "v2.5-beta" =~ $re ]]
    check "rematch whole" "${BASH_REMATCH[0]}" "v2.5-beta"
    check "rematch major" "${BASH_REMATCH[1]}" "2"
    check "rematch minor" "${BASH_REMATCH[2]}" "5"
    check "rematch suffix" "${BASH_REMATCH[4]}" "beta"
}

# ----------------------------------------------------------------------------
# Array churn, building indexed and associative arrays, slicing, listing the
# subscripts, and folding a checksum over the elements.
# ----------------------------------------------------------------------------
probe_array_churn() {
    announce "the array churn"
    local round i checksum=0
    local -a items window
    for ((round = 0; round < SCALE; round++)); do
        items=()
        for ((i = 0; i < 16; i++)); do
            items+=("e$((round * 16 + i))")
        done
        window=("${items[@]:4:8}")
        checksum=$((checksum + ${#items[@]} + ${#window[@]}))
        WORK_UNITS=$((WORK_UNITS + 1))
    done
    check "churn item total" "$checksum" "$((SCALE * 24))"

    # Feature coverage, exercised once.
    local -a base=(zero one two three four five)
    check "slice middle" "${base[*]:2:3}" "two three four"
    check "slice negative" "${base[*]: -2}" "four five"
    check "subscripts" "${!base[*]}" "0 1 2 3 4 5"
    base+=(six seven)
    check "append length" "${#base[@]}" "8"
    local -a sparse=([2]=a [5]=b [9]=c)
    check "sparse subscripts" "${!sparse[*]}" "2 5 9"
    check "sparse count" "${#sparse[@]}" "3"
    local -a removable=(p q r s t)
    unset 'removable[2]'
    check "dense unset" "${removable[*]}" "p q s t"
}

# ----------------------------------------------------------------------------
# Arithmetic, exponent, bases, C-style for, let, and the (( )) status.
# ----------------------------------------------------------------------------
probe_arithmetic() {
    announce "the arithmetic mill"
    local round i acc=0
    for ((round = 0; round < SCALE; round++)); do
        for ((i = 1; i <= 24; i++)); do
            acc=$(((acc + i * i - (i << 1) + (i ^ round)) % 7919))
        done
        WORK_UNITS=$((WORK_UNITS + 1))
    done
    check "arithmetic acc bounded" "$((acc >= 0 && acc < 7919 ? 1 : 0))" "1"

    check "exponent" "$((2 ** 10))" "1024"
    check "exponent precedence" "$((-2 ** 2))" "4"
    check "right assoc power" "$((2 ** 3 ** 2))" "512"
    check "hex base" "$((0xff + 16#10))" "271"
    check "octal base" "$((010 + 2#101))" "13"
    check "base 36" "$((36#z))" "35"
    check "ternary chain" "$((5 > 3 ? 5 < 100000 ? 1 : 2 : 3))" "1"
    check "bitwise mix" "$((6 & 3 | 8 ^ 1))" "11"

    local n=5
    ((n += 3, n *= 2, n -= 1))
    check "compound comma" "$n" "15"

    local status=ok
    ((0)) || status=zero-false
    check "double paren status" "$status" "zero-false"

    local fact=1
    for ((i = 1; i <= 6; i++)); do let "fact *= i"; done
    check "let factorial" "$fact" "720"

    local pre=4 post=4
    check "pre increment" "$((++pre))" "5"
    check "post increment" "$((post++)) $post" "4 5"
}

# ----------------------------------------------------------------------------
# A case state machine with alternatives, fall-through ;&, and continue-match
# ;;&, driven over a SCALE loop.
# ----------------------------------------------------------------------------
probe_case_machine() {
    announce "the case machine"
    local round k state=0 trail_len=0
    for ((round = 0; round < SCALE; round++)); do
        k=$((round % 6))
        case $k in
        0 | 1) state=$((state + 1)) ;;
        2) state=$((state + 2)) ;&
        3) state=$((state + 4)) ;;
        4) state=$((state * 2)) ;;&
        5) state=$((state + 1)) ;;
        esac
        state=$((state % 100003))
        trail_len=$((trail_len + 1))
        WORK_UNITS=$((WORK_UNITS + 1))
    done
    check "machine ran" "$((trail_len == SCALE ? 1 : 0))" "1"
    check "machine state bounded" "$((state >= 0 && state < 100003 ? 1 : 0))" "1"

    local marks=""
    case hello in
    h*) marks+="1" ;;&
    *o) marks+="2" ;;&
    hello) marks+="3" ;;
    *) marks+="x" ;;
    esac
    check "continue match" "$marks" "123"

    local fall=""
    case 1 in
    1) fall+="one" ;&
    2) fall+="two" ;;
    3) fall+="three" ;;
    esac
    check "fall through" "$fall" "onetwo"
}

# ----------------------------------------------------------------------------
# printf with the q quoting, the b escapes, several conversions, width, and
# assignment into a variable, folded over a SCALE loop.
# ----------------------------------------------------------------------------
probe_printf() {
    local round cell acc_len=0
    for ((round = 0; round < SCALE; round++)); do
        printf -v cell '%04d:%x:%o:%c' "$round" "$round" "$round" "A"
        acc_len=$((acc_len + ${#cell}))
        WORK_UNITS=$((WORK_UNITS + 1))
    done
    check "printf -v accumulated" "$((acc_len > 0 ? 1 : 0))" "1"

    check "printf quote space" "$(printf '%q' 'a b')" 'a\ b'
    check "printf quote special" "$(printf '%q' 'a$b;c')" 'a\$b\;c'
    check "printf b escapes" "$(printf '%b' 'x\ty')" "$(printf 'x\ty')"
    check "printf reuse" "$(printf '[%d]' 1 2 3)" "[1][2][3]"
    check "printf width" "$(printf '%5d' 42)" "   42"
    check "printf hex upper" "$(printf '%X' 255)" "FF"
    check "printf signed" "$(printf '%+d' 7)" "+7"
}

# ----------------------------------------------------------------------------
# ANSI-C quoting, measured by length and value so no raw control byte lands in
# the output and the comparison stays on printable text.
# ----------------------------------------------------------------------------
probe_ansi_c() {
    local tab=$'\t' newline=$'\n' esc=$'\e' ctrl=$'\cA'
    local hex=$'\x41\x42\x43' oct=$'\101\102' uni=$'A' mixed=$'A\x42C'
    check "tab length" "${#tab}" "1"
    check "newline length" "${#newline}" "1"
    check "escape length" "${#esc}" "1"
    check "control length" "${#ctrl}" "1"
    check "hex escape" "$hex" "ABC"
    check "octal escape" "$oct" "AB"
    check "unicode escape" "$uni" "A"
    check "mixed escape" "$mixed" "ABC"
}

# ----------------------------------------------------------------------------
# Parameter expansion feature coverage, the default and alternate forms,
# indirection, and the name listing, exercised once.
# ----------------------------------------------------------------------------
probe_param_features() {
    local word="configure-script-runner" empty=
    check "prefix removal" "${word%%-*}" "configure"
    check "suffix removal" "${word##*-}" "runner"
    check "replace all" "${word//-/_}" "configure_script_runner"
    check "replace first" "${word/-/_}" "configure_script-runner"
    check "anchor head" "${word/#configure/setup}" "setup-script-runner"
    check "anchor tail" "${word/%runner/walker}" "configure-script-walker"
    check "uppercase" "${word^^}" "CONFIGURE-SCRIPT-RUNNER"
    local upper=${word^^}
    check "lowercase" "${upper,,}" "configure-script-runner"
    check "first upper" "${word~}" "Configure-script-runner"
    check "substring" "${word:10:6}" "script"
    check "substring neg" "${word: -6}" "runner"
    check "length" "${#word}" "23"

    local ref=word
    check "indirection" "${!ref}" "configure-script-runner"
    local prefix_alpha=1 prefix_beta=2
    check "name listing" "${!prefix_*}" "prefix_alpha prefix_beta"
    check "default unset" "${undefined_var:-fallback}" "fallback"
    check "alt set" "${word:+present}" "present"
    check "alt empty" "${empty:+present}" ""
    check "assign default" "${unassigned:=seeded}" "seeded"
    check "assign took" "$unassigned" "seeded"
}

# ----------------------------------------------------------------------------
# Associative arrays, keys, values, and the -v existence test.
# ----------------------------------------------------------------------------
probe_assoc() {
    local -A capital=()
    local -a names=(france japan italy spain germany norway)
    local round n
    for ((round = 0; round < SCALE; round++)); do
        for n in "${names[@]}"; do
            capital[$n]=city_$((round % 11))
        done
        WORK_UNITS=$((WORK_UNITS + 1))
    done
    check "assoc key count" "${#capital[@]}" "${#names[@]}"

    local present=no
    [[ -v capital[france] ]] && present=yes
    check "assoc has france" "$present" "yes"
    local missing=no
    [[ -v capital[nowhere] ]] || missing=yes
    check "assoc lacks nowhere" "$missing" "yes"
}

# ----------------------------------------------------------------------------
# The [[ ]] conditional operators, string ordering, pattern matching, and
# grouping, exercised once.
# ----------------------------------------------------------------------------
probe_conditional() {
    local glob=no
    [[ "configure.bash" == *.bash ]] && glob=yes
    check "glob pattern" "$glob" "yes"
    local literal=no
    [[ "a*c" == "a*c" ]] && literal=yes
    check "quoted literal" "$literal" "yes"
    local order=no
    [[ "apple" < "banana" ]] && order=yes
    check "string order" "$order" "yes"
    local grouped=no
    [[ -n "x" && ( -z "" || "a" == "a" ) ]] && grouped=yes
    check "grouped tests" "$grouped" "yes"
    local numeric=no
    [[ 10 -gt 5 && 3 -le 3 ]] && numeric=yes
    check "numeric compare" "$numeric" "yes"
    local extglob=no
    [[ "abbbc" == a+(b)c ]] && extglob=yes
    check "extglob plus" "$extglob" "yes"
    local star=no
    [[ "ac" == a*(b)c ]] && star=yes
    check "extglob star" "$star" "yes"
    local neg=""
    case foo in
    !(bar)) neg="not-bar" ;;
    esac
    check "extglob negate case" "$neg" "not-bar"
}

# ----------------------------------------------------------------------------
# Brace expansion into arrays, then a count of the expanded fields, folded over
# a SCALE loop.
# ----------------------------------------------------------------------------
probe_brace() {
    local round total=0
    for ((round = 0; round < SCALE; round++)); do
        local -a batch=(f{1..8} g{a..d})
        total=$((total + ${#batch[@]}))
        WORK_UNITS=$((WORK_UNITS + 1))
    done
    check "brace batch total" "$total" "$((SCALE * 12))"

    check "char range" "$(echo {a..e})" "a b c d e"
    check "stepped range" "$(echo {1..10..2})" "1 3 5 7 9"
    check "descending range" "$(echo {5..1})" "5 4 3 2 1"
    check "nested list" "$(echo {a,{b,c},d})" "a b c d"
    check "prefix suffix" "$(echo x{1,2}y)" "x1y x2y"
    check "padded range" "$(echo {01..03})" "01 02 03"
}

# ----------------------------------------------------------------------------
# Functions, local scope, recursion, and return codes.
# ----------------------------------------------------------------------------
fib() {
    local n="$1"
    if ((n < 2)); then
        echo "$n"
        return 0
    fi
    local a b
    a=$(fib $((n - 1)))
    b=$(fib $((n - 2)))
    echo "$((a + b))"
}

function classify {
    local value="$1"
    if ((value % 2 == 0)); then return 0; fi
    return 1
}

probe_functions() {
    check "recursion fib 12" "$(fib 12)" "144"
    local round evens=0
    for ((round = 0; round < SCALE; round++)); do
        if classify "$round"; then evens=$((evens + 1)); fi
        WORK_UNITS=$((WORK_UNITS + 1))
    done
    check "even count" "$evens" "$(((SCALE + 1) / 2))"
}

# ----------------------------------------------------------------------------
# getopts over a fixed argument list.
# ----------------------------------------------------------------------------
probe_getopts() {
    local OPTIND=1 opt summary=""
    set -- -a -b -c value
    while getopts "abc:" opt; do
        case $opt in
        a) summary+="A" ;;
        b) summary+="B" ;;
        c) summary+="C(${OPTARG})" ;;
        esac
    done
    check "getopts summary" "$summary" "ABC(value)"
}

# ----------------------------------------------------------------------------
# One-shot probes that fork lightly, run once so they do not skew the timing.
# ----------------------------------------------------------------------------
probe_oneshot() {
    local -a lines=()
    mapfile -t lines <<< $'alpha\nbeta\ngamma'
    check "mapfile count" "${#lines[@]}" "3"
    check "mapfile first" "${lines[0]}" "alpha"
    local -a capped=()
    mapfile -t -n 2 capped <<< $'one\ntwo\nthree\nfour'
    check "mapfile capped" "${#capped[@]}" "2"
    local doc
    doc=$(cat <<EOF
heredoc body
EOF
)
    check "heredoc body" "$doc" "heredoc body"
    local hs
    read -r hs <<< "here string"
    check "here string" "$hs" "here string"
}

# ----------------------------------------------------------------------------
# Dead configuration branches. Every guard in the hot loop reads a constant
# toggle, a literal test, a bare false, or a never-entered while, all provable
# at parse time, so an optimizing shell picks the live path once while bash
# re-tests every guard on every pass. The live tail carries constant
# arithmetic the optimizer folds the same way.
# ----------------------------------------------------------------------------
probe_dead_configuration() {
    announce "the dead branches"
    local i live=0 limit=$((SCALE * SCALE / 2))
    for ((i = 1; i <= limit; i++)); do
        if [ "$CONFIG_TRACE" -eq 1 ]; then
            printf 'trace: %d\n' "$i"
            live=$((live + 1000))
        fi
        if [ "$CONFIG_PARANOID" -eq 1 ]; then
            live=$((live + i * i))
        fi
        if [ 0 -eq 1 ]; then
            live=$((live * 3))
        fi
        if false; then
            live=$((live + 7))
        fi
        while [ 0 -ne 0 ]; do
            live=$((live + 1))
        done
        if [ "$CONFIG_RELEASE" -eq 0 ]; then
            live=$((live - 1))
        elif [ "$CONFIG_PROFILE" -eq 1 ]; then
            live=$((live - 2))
        else
            live=$((live + (1 << 4) - (64 / 4) + 3 * 7 - 20))
        fi
        WORK_UNITS=$((WORK_UNITS + 1))
    done
    check "dead branches stayed dead" "$live" "$limit"
    check "release toggle held" "$CONFIG_RELEASE" "1"
    check "trace toggle held" "$CONFIG_TRACE" "0"
}

# ----------------------------------------------------------------------------
# Expansion extras, the wider parameter and array surface with one check per
# form, so the suite asserts more of the dialect per run.
# ----------------------------------------------------------------------------
probe_expansion_extras() {
    announce "the expansion extras"
    local word="configure-me-gently" ref=word
    check "indirect expansion" "${!ref}" "configure-me-gently"
    check "substring middle" "${word:10:2}" "me"
    check "negative offset" "${word: -6:3}" "gen"
    check "replace first" "${word/-/_}" "configure_me-gently"
    check "replace all" "${word//-/_}" "configure_me_gently"
    check "anchored replace" "${word/#configure/setup}" "setup-me-gently"
    check "anchored tail" "${word/%gently/firmly}" "configure-me-firmly"
    local -a slice=(zero one two three four five)
    check "array slice" "${slice[*]:2:3}" "two three four"
    check "array tail" "${slice[*]: -2}" "four five"
    local joined
    printf -v joined '%s|' "${slice[@]:1:2}"
    check "printf -v join" "$joined" "one|two|"
    check "case toggle one" "${word^}" "Configure-me-gently"
    check "length of slice" "${#slice[@]}" "6"
}

main() {
    printf 'configuring with the bash feature suite, SCALE=%s\n' "$SCALE"
    printf '%s\n' "----------------------------------------------------------------"

    probe_coprime_field
    probe_string_forge
    probe_regex_scan
    probe_array_churn
    probe_arithmetic
    probe_case_machine
    probe_printf
    probe_ansi_c
    probe_param_features
    probe_assoc
    probe_conditional
    probe_brace
    probe_functions
    probe_getopts
    probe_oneshot
    probe_dead_configuration
    probe_expansion_extras

    printf '%s\n' "----------------------------------------------------------------"
    printf 'configure complete: %d passed, %d failed, %d work units\n' \
        "$PASS_COUNT" "$FAIL_COUNT" "$WORK_UNITS"
}

main
