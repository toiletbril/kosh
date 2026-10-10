#!/bin/bash
# In bash mode, an argument shaped like an assignment expands a tilde after
# its first equals sign and after each colon, while an argument that does not
# start with a variable name keeps the tilde, as in bash. The home directory
# is printed as HOME so the output does not depend on the account.
for word in a=~/b a=~:~/d x[1]=~/e --p=~/c a=b:~/f "a=~/quoted" a=x~/g; do
  printf '%s\n' "${word//"$HOME"/HOME}"
done
printf '%s\n' path=~ other=~/one:~/two | while IFS= read -r line; do
  printf '%s\n' "${line//"$HOME"/HOME}"
done
