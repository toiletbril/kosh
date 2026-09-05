# This sourced helper exposes its BASH_ARGC and BASH_ARGV frame and one nested
# function frame to the call-stack compatibility fixture.
printf 'bash-source argc=<%s> argv=<%s> positional=<%s>\n' \
  "${BASH_ARGC[*]}" "${BASH_ARGV[*]}" "$*"
bash_argument_source_function() {
  printf 'bash-source-function argc=<%s> argv=<%s>\n' \
    "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
}
bash_argument_source_function Q1 Q2
unset -f bash_argument_source_function
