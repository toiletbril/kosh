#!/bin/bash
# A multibyte character in IFS splits and joins as one character in a UTF-8
# locale, in word splitting, read, and "$*", as in bash.
export LC_ALL=C.UTF-8
IFS=é
x='aébéé'
printf '<%s>' $x; echo
x='éa'
printf '<%s>' $x; echo
read -r a b <<< 'xéyéz'
echo "$a|$b"
set -- a b
printf '<%s>' "$*"; echo
array=(one two)
printf '<%s>' "${array[*]}"; echo
IFS='éx'
x=aébxc
printf '<%s>' $x; echo
read -r -a parts <<< 'péqér'
printf '<%s>' "${parts[@]}"; echo
