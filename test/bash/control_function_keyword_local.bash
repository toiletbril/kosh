#!/bin/bash
# function keyword definitions, nested functions, local recursion, and return codes.

function greet {
    echo "greet $1"
    return 0
}

function pick {
    if [ "$1" -gt 5 ]; then
        return 11
    fi
    return 22
}

greet there
pick 9
echo "rc $?"
pick 2
echo "rc $?"

# local keeps recursion frames independent.
function fib {
    local n=$1
    if [ "$n" -lt 2 ]; then
        echo "$n"
        return
    fi
    local a
    local b
    a=$(fib $((n - 1)))
    b=$(fib $((n - 2)))
    echo $((a + b))
}

echo "fib 10 = $(fib 10)"

# nested function defined inside another.
function build {
    function helper {
        echo "helper got $1"
        return 0
    }
    helper "$1"
    echo "helper rc $?"
}

build cargo
echo "build rc $?"

# local does not leak to caller.
function set_local {
    local scoped=private
    echo "inside $scoped"
}
scoped=public
set_local
echo "outside $scoped"


# Bash C-style for loop, for (( init; cond; step )), checked byte-for-byte
# against bash. Covers counting up and down, an accumulator, an empty header with
# break, continue and break inside, the comma operator, and a variable bound.
for (( i=0; i<3; i++ )); do echo "up $i"; done
for (( i=3; i>0; i-- )); do echo "down $i"; done
sum=0
for (( i=1; i<=10; i++ )); do sum=$((sum + i)); done
echo "sum $sum"
for (( ; ; )); do echo once; break; done
for (( i=0; i<5; i++ )); do
  if (( i == 2 )); then continue; fi
  echo "skip2 $i"
done
for (( i=0; i<10; i++ )); do
  if (( i == 3 )); then break; fi
  echo "stop3 $i"
done
for (( i=0, j=6; i<j; i++, j-- )); do echo "pair $i $j"; done
limit=4
for (( k=0; k<limit; k++ )); do echo "var $k"; done

for v in color color=always other; do
  case $v in
    (color|color=always) echo "matched: $v" ;;
    foo=bar) echo "fb: $v" ;;
    *) echo "default: $v" ;;
  esac
done

for x in a b c d; do
  case $x in
    a) echo "got a" ;;
    b) echo "got b" ;&
    c) echo "fellthrough or c" ;;
    d) echo "got d" ;;
  esac
done
echo "--- continue match ---"
case hello in
  h*) echo "starts h" ;;&
  *o) echo "ends o" ;;&
  hello) echo "exact" ;;
  *) echo "default" ;;
esac
echo "--- fall to last ---"
case 1 in
  1) echo one ;&
  2) echo two ;;
esac

# A keyword such as esac serves as the matched word after case, the lines the
# bash suite runs in case.tests.
case esac in (esac) echo esac-matched;; esac
case for in for) echo for-matched;; *) echo no;; esac
x=if
case $x in if) echo if-matched;; esac

# set -o pipefail effect on pipeline $?, and the C-style for loop.

set -o pipefail
false | true
echo "pipefail false|true rc $?"
true | false
echo "pipefail true|false rc $?"
true | true
echo "pipefail true|true rc $?"
set +o pipefail
false | true
echo "no pipefail false|true rc $?"

for (( i = 0; i < 5; i++ )); do
    echo "cfor $i"
done

sum=0
for (( k = 1; k <= 4; k++ )); do
    sum=$((sum + k))
done
echo "cfor sum $sum"

# nested C-style for with break N.
for (( a = 0; a < 3; a++ )); do
    for (( b = 0; b < 3; b++ )); do
        if (( a == 1 && b == 1 )); then
            break 2
        fi
        echo "cell $a $b"
    done
done
echo "done"

# Bash select loop, checked byte-for-byte against bash. The menu and prompt go to
# standard error, so a piped choice leaves only the body output on stdout. Covers
# a valid pick, an out-of-range pick, a non-number pick, iteration, and an empty
# line that just reprompts.
printf '1\n' | { select x in apple banana cherry; do echo "you picked $x"; break; done; }
printf '2\n' | { select c in red green blue; do echo "color is $c"; break; done; }
printf '99\n' | { select x in a b; do echo "out of range gives [$x]"; break; done; }
printf 'notnum\n' | { select x in a b; do echo "reply=$REPLY name=[$x]"; break; done; }
printf '1\n2\n3\n' | { select item in one two three; do echo "item: $item"; done; }
printf '\n3\n' | { select x in p q r; do echo "after empty: $x"; break; done; }
printf '' | { select x in p q; do :; done; }; echo "eof-status=$?"

eval 'for x "in" a; do :; done'
echo "quoted-for-in-status=$?"
eval 'case x "in" x) :;; esac'
echo "quoted-case-in-status=$?"
