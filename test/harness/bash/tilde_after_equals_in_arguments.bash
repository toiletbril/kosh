#!/bin/bash
# In bash mode, an argument shaped like an assignment expands a tilde after
# its first equals sign and after each colon, while an argument that does not
# start with a variable name keeps the tilde, as in bash.
HOME=/home/tilde_case
echo a=~/b a=~:~/d x[1]=~/e --p=~/c a=b:~/f "a=~/quoted" a=x~/g
printf '%s\n' path=~ other=~/one:~/two
