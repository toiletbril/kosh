#!/bin/bash
# read -N reads exactly the given byte count past any delimiter, keeps the
# text unsplit and untrimmed in the first name, still removes backslashes
# without -r, and fails when the input ends first, as in bash.
printf "ab cd\nef" | { read -N 4 a b; echo "rc=$? a=[$a] b=[$b]"; read -N 10 c; echo "rc=$? c=[$c]"; }
printf " x \n" | { read -N 3 a; echo "[$a]"; }
printf "a\\\\bc" | { read -N 3 a; echo "[$a]"; }
printf "a\\\\bc" | { read -r -N 3 a; echo "[$a]"; }
printf "xyz" | { read -N 2; echo "[$REPLY]"; }
