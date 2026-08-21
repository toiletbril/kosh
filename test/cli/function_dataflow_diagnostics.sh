#!/bin/sh

unset KOSH_FLAGS

echo '== a call passes nothing to a body that reads its arguments:'
"$BIN" -n -WWW -c 'greet() {
  echo "hello $1"
}
greet' 2>&1

echo '== one call with arguments clears every call of that function:'
"$BIN" -n -WWW -c 'greet() {
  echo "hello $1"
}
greet world
greet' 2>&1
echo "rc=$?"

echo '== a body that reads no argument stays quiet:'
"$BIN" -n -WWW -c 'greet() {
  echo hello
}
greet' 2>&1
echo "rc=$?"

echo '== a definition that no call reaches stays quiet:'
"$BIN" -n -WWW -c 'greet() {
  echo "hello $1"
}' 2>&1
echo "rc=$?"

echo '== a modifier supplies the value, so the read does not count:'
"$BIN" -n -WWW -c 'greet() {
  echo "hello ${1:-world}"
}
greet' 2>&1
echo "rc=$?"

echo '== a positional read in a test operand counts:'
"$BIN" -n -WWW -c 'check() {
  [ -n "$1" ]
}
check' 2>&1

echo '== a positional read in an arithmetic expression counts:'
"$BIN" -n -WWW -c 'double() {
  echo $((2 * $1))
}
double' 2>&1

echo '== a positional read in a for loop word counts:'
"$BIN" -n -WWW -c 'walk() {
  for one in "$@"; do echo "$one"; done
}
walk' 2>&1

echo '== a positional star read names its exact expansion:'
"$BIN" -n -WWW -c 'join() {
  printf "%s\n" "$*"
}
join' 2>&1

echo '== the first reading definition remains authoritative:'
"$BIN" -n -WWW -c 'choice() {
  echo "$1"
}
choice() {
  echo plain
}
choice' 2>&1

echo '== a later reading definition does not replace the first summary:'
"$BIN" -n -WWW -c 'choice() {
  echo plain
}
choice() {
  echo "$1"
}
choice' 2>&1
echo "rc=$?"

echo '== a call runs before the definition:'
"$BIN" -n -WWW -c 'later_helper foo
later_helper() {
  echo "$1"
}' 2>&1

echo '== a call to a builtin name defined later stays quiet:'
"$BIN" -n -WWW -c 'exit 0
function exit {
  builtin exit "$@"
}' 2>&1
echo "rc=$?"

echo '== a definition above the call stays quiet:'
"$BIN" -n -WWW -c 'early_helper() {
  echo "$1"
}
early_helper foo' 2>&1
echo "rc=$?"

echo '== a call inside another body stays quiet:'
"$BIN" -n -WWW -c 'outer() {
  inner foo
}
inner() {
  echo "$1"
}
outer' 2>&1
echo "rc=$?"

echo '== the default mood rejects a call that passes nothing:'
"$BIN" -n -c 'greet() {
  echo "hello $1"
}
greet' 2>&1
echo "rc=$?"

echo '== a disable directive silences the sweep:'
"$BIN" -n -WWW -c '# shellcheck disable=SC2119,SC2120
greet() {
  echo "hello $1"
}
greet' 2>&1
echo "rc=$?"
