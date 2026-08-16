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
