#!/bin/bash
# FUNCNAME reads the call stack, the scalar the innermost frame, the array the
# whole stack, and the name is unset outside a function.
f() { echo "in=$FUNCNAME"; g; }
g() { echo "g0=${FUNCNAME[0]} g1=${FUNCNAME[1]} depth=${#FUNCNAME[@]}"; }
f
echo "outside=[${FUNCNAME-unset}]"
h() { for fr in "${FUNCNAME[@]}"; do echo "frame=$fr"; done; }
h
k() { builtin eval -- "function $FUNCNAME/sub { echo subbed; }"; "$FUNCNAME/sub"; }
k

# BASH_ARGC and BASH_ARGV expose immutable call-entry arguments while extdebug
# tracks active function frames.
bash_argument_source_path=bash/goldens/bash_argument_source.bash
(
  set -- root-one 'root two'
  source "$bash_argument_source_path"
  printf 'bash-source-return argc=<%s> argv=<%s>\n' \
    "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
)
(
  set -- root-one 'root two'
  source "$bash_argument_source_path" S1 S2
  printf 'bash-source-args-return argc=<%s> argv=<%s>\n' \
    "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
)
bash_args_snapshot() {
  (shopt -s extdebug
   printf 'bash-args-snapshot-sub argc=<%s> argv=<%s>\n' \
     "${BASH_ARGC[*]}" "${BASH_ARGV[*]}")
}
bash_args_snapshot snapshot-one 'snapshot two'
printf 'bash-args-off argc=<%s> argv=<%s>\n' \
  "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
shopt -s extdebug
bash_args_outer() {
  bash_args_inner 'inner-one' 'inner two'
}
bash_args_inner() {
  printf 'bash-argc=<%s> count=%s scalar=<%s>\n' \
    "${BASH_ARGC[*]}" "${#BASH_ARGC[@]}" "$BASH_ARGC"
  printf 'bash-argv=<%s> count=%s scalar=<%s>\n' \
    "${BASH_ARGV[*]}" "${#BASH_ARGV[@]}" "$BASH_ARGV"
  local BASH_ARGC BASH_ARGV 2>/dev/null
  printf 'bash-args-local-status=%s argc=<%s> argv=<%s>\n' \
    "$?" "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
  local 'BASH_ARGC[0]' 2>/dev/null
  printf 'bash-argc-element-local-status=%s argc=<%s>\n' \
    "$?" "${BASH_ARGC[*]}"
  BASH_ARGC[0]=99
  printf 'bash-argc-write-status=%s value=<%s>\n' "$?" "${BASH_ARGC[*]}"
  BASH_ARGV[0]=changed
  printf 'bash-argv-write-status=%s value=<%s>\n' "$?" "${BASH_ARGV[*]}"
  unset 'BASH_ARGC[0]' 2>/dev/null
  printf 'bash-argc-unset-status=%s value=<%s>\n' "$?" "${BASH_ARGC[*]}"
  unset BASH_ARGV 2>/dev/null
  printf 'bash-argv-unset-status=%s value=<%s>\n' "$?" "${BASH_ARGV[*]}"
  printf 'bash-args-negative argc=<%s> argv=<%s>\n' \
    "${BASH_ARGC[-1]}" "${BASH_ARGV[-1]}"
  (printf '%s\n' "${BASH_ARGC[-9]}") 2>/dev/null
  printf 'bash-args-bad-negative-status=%s\n' "$?"
}
bash_args_outer 'outer-one' 'outer two'
printf 'bash-args-return argc=<%s> argv=<%s>\n' \
  "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
declare -a BASH_ARGC=(9)
declare -a BASH_ARGV=(changed)
printf 'bash-args-redeclare argc=<%s> argv=<%s>\n' \
  "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"

source "$bash_argument_source_path"
source "$bash_argument_source_path" S1 S2
unset bash_argument_source_path

shopt -u extdebug
bash_args_toggle_outer() {
  shopt -s extdebug
  printf 'bash-toggle-outer argc=<%s> argv=<%s>\n' \
    "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
  bash_args_toggle_inner I1 I2 I3
  printf 'bash-toggle-after-inner argc=<%s> argv=<%s>\n' \
    "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
  shopt -s extdebug
}
bash_args_toggle_inner() {
  printf 'bash-toggle-inner argc=<%s> argv=<%s>\n' \
    "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"
  shopt -u extdebug
}
bash_args_toggle_outer O1 O2
printf 'bash-toggle-return argc=<%s> argv=<%s>\n' \
  "${BASH_ARGC[*]}" "${BASH_ARGV[*]}"


# FUNCNAME reads inside a function in its scalar and array forms, and OSTYPE
# reads the platform, the dynamic variables a sourced config relies on.
outer() {
  echo "scalar=${FUNCNAME}"
  echo "zero=${FUNCNAME[0]}"
  inner
}
inner() {
  echo "depth=${#FUNCNAME[@]}"
  echo "stack=${FUNCNAME[*]}"
}
outer
[[ $OSTYPE == linux* || $OSTYPE == darwin* || $OSTYPE == msys* ]] &&
  echo "ostype=known"
f() { unset -f "$FUNCNAME"; }
f
command -v f >/dev/null || echo "unset_self=gone"

# $_ reads the last argument of the previous simple command, checked
# byte-for-byte against bash.
true alpha beta
echo "after_true=[$_]"
echo one two three
echo "after_echo=[$_]"
:
echo "after_colon=[$_]"
printf '%s\n' x y z
echo "after_printf=[$_]"

underscore_function() {
  printf 'function-entry=[%s]\n' "$_"
  : function-body
}
: before-function
underscore_function call-tail
printf 'function-after=[%s]\n' "$_"

: snapshot-parent
printf 'snapshot-sub=[%s] snapshot-underscore=[%s]\n' \
  "$(: snapshot-child; printf child-output)" "$_"

r=$EPOCHREALTIME
[[ $r == *.* ]] && echo "has-dot"
frac=${r#*.}
echo "frac-len: ${#frac}"
sec=${r%.*}
case $sec in
  [0-9]*) echo "sec-numeric" ;;
  *) echo "sec-bad" ;;
esac
echo "base: $((10#0${r%.*} > 0))"
