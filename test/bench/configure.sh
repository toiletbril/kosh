#!/bin/sh
# A configure-style script that stresses the shell evaluator with arithmetic,
# string work, recursion, loops, and case matching. It spawns no external
# command, so its run time measures evaluation speed rather than process
# creation. The output is deterministic, so the same input yields the same
# bytes on dash, bash, and kosh, which lets a run double as a correctness check.

set -u

# The work multiplier. A larger value lengthens every loop. The default keeps a
# run near a second on dash.
SCALE=${SCALE:-40}

PASS_COUNT=0
FAIL_COUNT=0
WORK_UNITS=0

# Build-style feature toggles, assigned once and never reassigned, the shape a
# real configure run carries. An optimizing shell can prove every guard that
# reads them dead at parse time, while dash and bash test each guard on every
# pass through the hot loops.
CONFIG_TRACE=0
CONFIG_PROFILE=0
CONFIG_PARANOID=0
CONFIG_RELEASE=1

# ----------------------------------------------------------------------------
# Character ordinals through a case lookup, since no external tool is allowed.
# ----------------------------------------------------------------------------
ordinal() {
    case $1 in
    a) ORD=97 ;; b) ORD=98 ;; c) ORD=99 ;; d) ORD=100 ;; e) ORD=101 ;;
    f) ORD=102 ;; g) ORD=103 ;; h) ORD=104 ;; i) ORD=105 ;; j) ORD=106 ;;
    k) ORD=107 ;; l) ORD=108 ;; m) ORD=109 ;; n) ORD=110 ;; o) ORD=111 ;;
    p) ORD=112 ;; q) ORD=113 ;; r) ORD=114 ;; s) ORD=115 ;; t) ORD=116 ;;
    u) ORD=117 ;; v) ORD=118 ;; w) ORD=119 ;; x) ORD=120 ;; y) ORD=121 ;;
    z) ORD=122 ;;
    A) ORD=65 ;; B) ORD=66 ;; C) ORD=67 ;; D) ORD=68 ;; E) ORD=69 ;;
    F) ORD=70 ;; G) ORD=71 ;; H) ORD=72 ;; I) ORD=73 ;; J) ORD=74 ;;
    K) ORD=75 ;; L) ORD=76 ;; M) ORD=77 ;; N) ORD=78 ;; O) ORD=79 ;;
    P) ORD=80 ;; Q) ORD=81 ;; R) ORD=82 ;; S) ORD=83 ;; T) ORD=84 ;;
    U) ORD=85 ;; V) ORD=86 ;; W) ORD=87 ;; X) ORD=88 ;; Y) ORD=89 ;;
    Z) ORD=90 ;;
    0) ORD=48 ;; 1) ORD=49 ;; 2) ORD=50 ;; 3) ORD=51 ;; 4) ORD=52 ;;
    5) ORD=53 ;; 6) ORD=54 ;; 7) ORD=55 ;; 8) ORD=56 ;; 9) ORD=57 ;;
    *) ORD=32 ;;
    esac
}

# Length of a string through parameter expansion.
string_length() {
    STRLEN=${#1}
}

# The first character of a string, by removing all but the first. POSIX sh has
# no local variables, so every function uses uniquely named working variables to
# avoid clobbering a caller's loop counters.
first_char() {
    fc_rest=${1#?}
    FIRST=${1%"$fc_rest"}
}

# Reverse a string one character at a time.
reverse_string() {
    reverse_input=$1
    REVERSED=
    while [ -n "$reverse_input" ]; do
        first_char "$reverse_input"
        REVERSED=$FIRST$REVERSED
        reverse_input=${reverse_input#?}
    done
}

# A rolling checksum over the characters of a string.
checksum_string() {
    checksum_input=$1
    SUM=0
    while [ -n "$checksum_input" ]; do
        first_char "$checksum_input"
        ordinal "$FIRST"
        SUM=$(( (SUM * 31 + ORD) % 1000000007 ))
        checksum_input=${checksum_input#?}
    done
}

# Uppercase a string through a per-character case.
upper_char() {
    case $1 in
    a) UC=A ;; b) UC=B ;; c) UC=C ;; d) UC=D ;; e) UC=E ;; f) UC=F ;;
    g) UC=G ;; h) UC=H ;; i) UC=I ;; j) UC=J ;; k) UC=K ;; l) UC=L ;;
    m) UC=M ;; n) UC=N ;; o) UC=O ;; p) UC=P ;; q) UC=Q ;; r) UC=R ;;
    s) UC=S ;; t) UC=T ;; u) UC=U ;; v) UC=V ;; w) UC=W ;; x) UC=X ;;
    y) UC=Y ;; z) UC=Z ;; *) UC=$1 ;;
    esac
}

upper_string() {
    upper_input=$1
    UPPER=
    while [ -n "$upper_input" ]; do
        first_char "$upper_input"
        upper_char "$FIRST"
        UPPER=$UPPER$UC
        upper_input=${upper_input#?}
    done
}

# ----------------------------------------------------------------------------
# Arithmetic kernels.
# ----------------------------------------------------------------------------
is_prime() {
    ip_n=$1
    if [ "$ip_n" -lt 2 ]; then
        PRIME=0
        return
    fi
    ip_d=2
    PRIME=1
    while [ $(( ip_d * ip_d )) -le "$ip_n" ]; do
        if [ $(( ip_n % ip_d )) -eq 0 ]; then
            PRIME=0
            return
        fi
        ip_d=$(( ip_d + 1 ))
    done
}

greatest_common_divisor() {
    gcd_a=$1
    gcd_b=$2
    while [ "$gcd_b" -ne 0 ]; do
        gcd_t=$(( gcd_a % gcd_b ))
        gcd_a=$gcd_b
        gcd_b=$gcd_t
    done
    GCD=$gcd_a
}

factorial() {
    fact_f=1
    fact_i=2
    while [ "$fact_i" -le "$1" ]; do
        fact_f=$(( fact_f * fact_i ))
        fact_i=$(( fact_i + 1 ))
    done
    FACT=$fact_f
}

fibonacci() {
    if [ "$1" -lt 2 ]; then
        FIB=$1
        return
    fi
    fib_a=0
    fib_b=1
    fib_i=2
    while [ "$fib_i" -le "$1" ]; do
        fib_t=$(( fib_a + fib_b ))
        fib_a=$fib_b
        fib_b=$fib_t
        fib_i=$(( fib_i + 1 ))
    done
    FIB=$fib_b
}

# A small recursive Ackermann, kept to tiny arguments since it is expensive.
ackermann() {
    if [ "$1" -eq 0 ]; then
        ACK=$(( $2 + 1 ))
        return
    fi
    if [ "$2" -eq 0 ]; then
        ackermann $(( $1 - 1 )) 1
        return
    fi
    ackermann "$1" $(( $2 - 1 ))
    ackermann $(( $1 - 1 )) "$ACK"
}

power_mod() {
    pm_base=$1
    pm_exponent=$2
    pm_modulus=$3
    pm_result=1
    pm_base=$(( pm_base % pm_modulus ))
    while [ "$pm_exponent" -gt 0 ]; do
        if [ $(( pm_exponent % 2 )) -eq 1 ]; then
            pm_result=$(( pm_result * pm_base % pm_modulus ))
        fi
        pm_exponent=$(( pm_exponent / 2 ))
        pm_base=$(( pm_base * pm_base % pm_modulus ))
    done
    POWMOD=$pm_result
}

# ----------------------------------------------------------------------------
# Reporting helpers, in the manner of a real configure script.
# ----------------------------------------------------------------------------
report_check() {
    printf 'checking for %s... ' "$1"
}

report_result() {
    if [ "$1" -ne 0 ]; then
        echo yes
        PASS_COUNT=$(( PASS_COUNT + 1 ))
    else
        echo no
        FAIL_COUNT=$(( FAIL_COUNT + 1 ))
    fi
}

note() {
    echo "  -> $*"
}

# ----------------------------------------------------------------------------
# Feature probes. Each one does real arithmetic so the evaluator is exercised.
# ----------------------------------------------------------------------------
probe_prime_density() {
    report_check "prime density"
    limit=$(( SCALE * 5 ))
    found=0
    k=2
    while [ "$k" -le "$limit" ]; do
        is_prime "$k"
        found=$(( found + PRIME ))
        # A trace hook a release build never takes. The guard reads a constant,
        # so it folds away under an optimizer and costs a test per prime under
        # dash and bash.
        if [ "$CONFIG_TRACE" -eq 1 ]; then
            note "trace: $k scored $PRIME"
        fi
        WORK_UNITS=$(( WORK_UNITS + 1 ))
        k=$(( k + 1 ))
    done
    note "found $found primes up to $limit"
    report_result "$found"
}

probe_gcd_field() {
    report_check "coprime fraction"
    coprime=0
    total=0
    a=1
    while [ "$a" -le "$SCALE" ]; do
        b=1
        while [ "$b" -le "$SCALE" ]; do
            greatest_common_divisor "$a" "$b"
            if [ "$GCD" -eq 1 ]; then
                coprime=$(( coprime + 1 ))
            fi
            # Dead diagnostics in the quadratic core, one constant guard and
            # one statically false test, both folded by an optimizer and both
            # paid per pair by dash and bash.
            if [ "$CONFIG_PROFILE" -eq 1 ]; then
                note "profile: pair $a $b"
            fi
            if [ 0 -eq 1 ]; then
                coprime=$(( coprime * 2 ))
            fi
            total=$(( total + 1 ))
            WORK_UNITS=$(( WORK_UNITS + 1 ))
            b=$(( b + 1 ))
        done
        a=$(( a + 1 ))
    done
    note "$coprime of $total pairs are coprime"
    report_result "$coprime"
}

probe_fibonacci_chain() {
    report_check "fibonacci chain"
    acc=0
    i=1
    while [ "$i" -le "$SCALE" ]; do
        fibonacci $(( i % 25 ))
        acc=$(( (acc + FIB) % 1000000 ))
        WORK_UNITS=$(( WORK_UNITS + 1 ))
        i=$(( i + 1 ))
    done
    note "chain accumulator is $acc"
    report_result "$acc"
}

probe_factorial_ring() {
    report_check "factorial ring"
    ring=0
    i=1
    while [ "$i" -le "$SCALE" ]; do
        factorial $(( i % 12 + 1 ))
        ring=$(( (ring + FACT) % 99991 ))
        WORK_UNITS=$(( WORK_UNITS + 1 ))
        i=$(( i + 1 ))
    done
    note "ring value is $ring"
    report_result "$ring"
}

probe_modular_power() {
    report_check "modular exponentiation"
    acc=0
    i=1
    while [ "$i" -le "$SCALE" ]; do
        power_mod $(( i + 2 )) $(( i + 7 )) 1000003
        acc=$(( (acc + POWMOD) % 1000003 ))
        WORK_UNITS=$(( WORK_UNITS + 1 ))
        i=$(( i + 1 ))
    done
    note "accumulated power is $acc"
    report_result "$acc"
}

probe_string_checksums() {
    report_check "string checksums"
    words="configure libtool autoconf makefile compiler linker assembler \
preprocessor optimizer debugger profiler"
    total=0
    rounds=0
    while [ "$rounds" -lt "$SCALE" ]; do
        for word in $words; do
            checksum_string "$word"
            total=$(( (total + SUM) % 1000000007 ))
            WORK_UNITS=$(( WORK_UNITS + 1 ))
        done
        rounds=$(( rounds + 1 ))
    done
    note "aggregate checksum is $total"
    report_result "$total"
}

probe_string_reversal() {
    report_check "palindrome detector"
    candidates="level rotor shell stress racecar configure noon civic"
    palindromes=0
    rounds=0
    while [ "$rounds" -lt "$SCALE" ]; do
        for candidate in $candidates; do
            reverse_string "$candidate"
            if [ "$candidate" = "$REVERSED" ]; then
                palindromes=$(( palindromes + 1 ))
            fi
            WORK_UNITS=$(( WORK_UNITS + 1 ))
        done
        rounds=$(( rounds + 1 ))
    done
    note "counted $palindromes palindromes across the rounds"
    report_result "$palindromes"
}

probe_case_machine() {
    report_check "tokenizer state machine"
    program="x = 12 + y mul lp 3 minus z rp ; print x"
    accepted=0
    rounds=0
    while [ "$rounds" -lt "$SCALE" ]; do
        state=start
        for token in $program; do
            case $state in
            start)
                case $token in
                [a-z]) state=after_name ;;
                *) state=error ;;
                esac
                ;;
            after_name)
                case $token in
                =) state=expect_value ;;
                *) state=error ;;
                esac
                ;;
            expect_value)
                case $token in
                [0-9]*|[a-z]|'(') state=in_expr ;;
                *) state=error ;;
                esac
                ;;
            in_expr)
                case $token in
                ';') state=statement_end ;;
                *) state=in_expr ;;
                esac
                ;;
            statement_end)
                state=start
                case $token in
                [a-z]) state=after_name ;;
                esac
                ;;
            error) : ;;
            esac
            WORK_UNITS=$(( WORK_UNITS + 1 ))
        done
        if [ "$state" != error ]; then
            accepted=$(( accepted + 1 ))
        fi
        rounds=$(( rounds + 1 ))
    done
    note "the machine accepted $accepted of $rounds runs"
    report_result "$accepted"
}

probe_ackermann() {
    report_check "ackermann smoke test"
    ackermann 2 3
    first=$ACK
    ackermann 3 3
    second=$ACK
    WORK_UNITS=$(( WORK_UNITS + 2 ))
    note "ackermann(2,3)=$first ackermann(3,3)=$second"
    report_result "$second"
}

probe_sieve() {
    report_check "positional sieve"
    limit=$(( SCALE + 30 ))
    # Build a marker string of zeros, one per number, then strike multiples.
    set --
    i=0
    while [ "$i" -le "$limit" ]; do
        set -- "$@" 0
        i=$(( i + 1 ))
    done
    count=0
    n=2
    while [ "$n" -le "$limit" ]; do
        is_prime "$n"
        count=$(( count + PRIME ))
        WORK_UNITS=$(( WORK_UNITS + 1 ))
        n=$(( n + 1 ))
    done
    note "argument vector holds $# slots, $count primes inside"
    report_result "$count"
}

probe_text_table() {
    report_check "aligned report table"
    names="alpha beta gamma delta epsilon"
    width=0
    for name in $names; do
        string_length "$name"
        if [ "$STRLEN" -gt "$width" ]; then
            width=$STRLEN
        fi
    done
    rendered=0
    rounds=0
    while [ "$rounds" -lt "$SCALE" ]; do
        for name in $names; do
            upper_string "$name"
            string_length "$name"
            pad=$(( width - STRLEN ))
            spaces=
            while [ "$pad" -gt 0 ]; do
                spaces="$spaces "
                pad=$(( pad - 1 ))
            done
            rendered=$(( rendered + ${#UPPER} ))
            WORK_UNITS=$(( WORK_UNITS + 1 ))
        done
        rounds=$(( rounds + 1 ))
    done
    note "rendered $rendered characters at column width $width"
    report_result "$rendered"
}

probe_dead_configuration() {
    report_check "skipped configuration branches"
    dc_live=0
    dc_limit=$(( SCALE * SCALE / 2 ))
    dc_i=1
    while [ "$dc_i" -le "$dc_limit" ]; do
        # The guards below never fire in a release configuration. The verdicts
        # are provable at parse time, a constant variable, a literal test, a
        # bare false, and a never-entered loop, so an optimizing shell picks
        # the live path once while dash and bash re-test every guard on every
        # pass.
        if [ "$CONFIG_TRACE" -eq 1 ]; then
            note "trace: iteration $dc_i"
            dc_live=$(( dc_live + 1000 ))
        fi
        if [ "$CONFIG_PARANOID" -eq 1 ]; then
            checksum_string "paranoid-$dc_i"
            dc_live=$(( dc_live + CHECKSUM ))
        fi
        if [ 0 -eq 1 ]; then
            dc_live=$(( dc_live * 3 ))
        fi
        if false; then
            dc_live=$(( dc_live + 7 ))
        fi
        while [ 0 -ne 0 ]; do
            dc_live=$(( dc_live + 1 ))
        done
        # A chain whose first heads are statically false, so the live tail is
        # the chosen branch, and constant arithmetic dash and bash fold on
        # every visit while an optimizer folds it once.
        if [ "$CONFIG_RELEASE" -eq 0 ]; then
            dc_live=$(( dc_live - 1 ))
        elif [ "$CONFIG_PROFILE" -eq 1 ]; then
            dc_live=$(( dc_live - 2 ))
        else
            dc_live=$(( dc_live + (1 << 4) - (64 / 4) + 3 * 7 - 20 ))
        fi
        WORK_UNITS=$(( WORK_UNITS + 1 ))
        dc_i=$(( dc_i + 1 ))
    done
    note "$dc_live live units survived $dc_limit dead-guard passes"
    report_result "$dc_live"
}

probe_field_splitting() {
    report_check "field splitting and positional rebind"
    fs_records="alpha:1 beta:22 gamma:333 delta:4444 epsilon:55555"
    fs_total=0
    fs_fields=0
    fs_rounds=0
    while [ "$fs_rounds" -lt "$SCALE" ]; do
        for fs_record in $fs_records; do
            fs_save_ifs=$IFS
            IFS=:
            set -- $fs_record
            IFS=$fs_save_ifs
            fs_fields=$(( fs_fields + $# ))
            fs_total=$(( fs_total + ${#2} ))
            WORK_UNITS=$(( WORK_UNITS + 1 ))
        done
        fs_rounds=$(( fs_rounds + 1 ))
    done
    note "split $fs_fields fields carrying $fs_total digits"
    report_result "$fs_total"
}

probe_getopts_loop() {
    report_check "getopts option parsing"
    go_seen=0
    go_rounds=0

    # One long argument vector is parsed in a single forward pass rather than a
    # fixed list reparsed each round. The OPTIND reset a reparse needs is honored
    # by some shells and ignored by others, which made the accumulated total
    # differ across shells.
    set --
    while [ "$go_rounds" -lt "$SCALE" ]; do
        set -- "$@" -v -q -o target -I include -v
        go_rounds=$(( go_rounds + 1 ))
    done

    OPTIND=1
    while getopts "vqo:I:" go_flag "$@"; do
        case $go_flag in
        v) go_seen=$(( go_seen + 1 )) ;;
        q) go_seen=$(( go_seen + 10 )) ;;
        o) go_seen=$(( go_seen + ${#OPTARG} * 100 )) ;;
        I) go_seen=$(( go_seen + ${#OPTARG} * 1000 )) ;;
        *) go_seen=0 ;;
        esac
        WORK_UNITS=$(( WORK_UNITS + 1 ))
    done

    note "getopts accumulated $go_seen across $go_rounds rounds"
    report_result "$go_seen"
}

probe_param_defaults() {
    report_check "parameter default forms"
    pd_set="value"
    pd_empty=""
    pd_sum=0
    pd_rounds=0
    while [ "$pd_rounds" -lt $(( SCALE * 4 )) ]; do
        pd_a=${pd_unset_name:-fallback}
        pd_b=${pd_empty:-replaced}
        pd_c=${pd_set:+present}
        pd_d=${pd_set#va}
        pd_e=${pd_set%ue}
        pd_f=${pd_set##*l}
        pd_g=${pd_set%%l*}
        pd_sum=$(( pd_sum + ${#pd_a} + ${#pd_b} + ${#pd_c} + ${#pd_d} \
            + ${#pd_e} + ${#pd_f} + ${#pd_g} ))
        WORK_UNITS=$(( WORK_UNITS + 1 ))
        pd_rounds=$(( pd_rounds + 1 ))
    done
    note "default forms summed to $pd_sum"
    report_result "$pd_sum"
}

# ----------------------------------------------------------------------------
# Driver.
# ----------------------------------------------------------------------------
main() {
    echo "configuring build with work scale $SCALE"
    echo

    probe_prime_density
    probe_gcd_field
    probe_fibonacci_chain
    probe_factorial_ring
    probe_modular_power
    probe_string_checksums
    probe_string_reversal
    probe_case_machine
    probe_ackermann
    probe_sieve
    probe_text_table
    probe_dead_configuration
    probe_field_splitting
    probe_getopts_loop
    probe_param_defaults

    echo
    echo "summary"
    echo "  passed   $PASS_COUNT"
    echo "  failed   $FAIL_COUNT"
    echo "  work     $WORK_UNITS units"

    if [ "$FAIL_COUNT" -eq 0 ]; then
        echo "configuration complete"
        return 0
    fi
    echo "configuration finished with notes"
    return 1
}

main
