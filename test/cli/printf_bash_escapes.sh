unset KOSH_FLAGS
# printf \e and \E emit the escape character in bash moods but pass through
# literally in the sh mood. The %b conversion follows the same gate.
output_sentinel=kosh-printf-output-end
escape_character=$(printf '\033')
escape_line=$(printf '\033\nx')
escape_line=${escape_line%x}
literal_escape='\e'
literal_escape_line=$(printf '\\e\nx')
literal_escape_line=${literal_escape_line%x}

check_output()
{
    expected_output=$1
    shift
    actual_output=$("$@"; printf %s "$output_sentinel")
    if [ "$actual_output" = "$expected_output$output_sentinel" ]; then
        printf 'matched\n'
    else
        printf 'mismatched\n'
    fi
}

printf '%s\n' '== bash mood printf \e:'
check_output "$escape_character" "$BIN" --mood bash -c 'printf "\e"'
printf '%s\n' '== bash mood printf \E:'
check_output "$escape_character" "$BIN" --mood bash -c 'printf "\E"'
printf '%s\n' '== sh mood printf \e stays literal:'
check_output "$literal_escape" "$BIN" --mood sh -c 'printf "\e"'
printf '%s\n' '== bash-posix mood printf \e:'
check_output "$escape_character" "$BIN" --mood bash-posix -c 'printf "\e"'
printf '%s\n' '== bash mood %b with \e:'
check_output "$escape_line" "$BIN" --mood bash -c 'printf "%b\n" "\e"'
printf '%s\n' '== sh mood %b with \e stays literal:'
check_output "$literal_escape_line" "$BIN" --mood sh -c \
    'printf "%b\n" "\e"'
