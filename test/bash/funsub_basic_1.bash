#!/bin/bash
# Alternative for funsub_basic.bash on a bash older than 5.3, which lacks the
# ${ command; } funsub. The body is rewritten with $(...) for the capturing
# forms and plain statements for the side-effecting ones, so the output matches
# kosh byte for byte.
echo "simple=$(echo one)"
echo "quoted=[$(printf 'a\nb\n\n\n')]"
persist=42
echo "persist=$persist"
pf(){ echo from_func; }
pf
echo "nested=$(echo $(echo inner))"
echo "split" $(echo a b c)
echo "joined=[$(echo x y z)]"
cd /tmp
echo "cwd=$(pwd)"
IFS=-
echo "ifs=$(echo p-q-r)"
unset IFS
