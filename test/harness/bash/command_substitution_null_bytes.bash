#!/bin/bash
# A command substitution drops null bytes from its output before removing
# trailing newlines, so the value compares, counts, and matches as in bash.
work=$(mktemp -d)
cd "$work" || exit 1
x=$(printf 'a\0b' 2> /dev/null)
echo "length=${#x}"
[ "$x" = ab ] && echo equal || echo different
case $x in ab) echo matched ;; *) echo unmatched ;; esac
printf 'c\0d\n' > nul.bin
y=$(< nul.bin)
echo "file=${#y}"
z=$(printf '\0\0')
echo "only-nulls=[$z] ${#z}"
w=$(printf 'tail\n\0\n\n')
echo "trailing=${#w}"
cd / && rm -rf -- "$work"
