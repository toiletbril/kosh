#!/bin/bash
compgen -W "alpha beta gamma alphabet" -- a
echo ---
compgen -W "one two three" -- t
echo ---
compgen -W "x y z" --

complete -W default -D
complete -W zed zed
complete -W alpha alpha
complete -p

complete -W "add commit push" mygit
echo "rc1=$?"
complete -o default -F _foo othercmd
echo "rc2=$?"
complete -W "a b" -F fn -o default name1 name2
echo "rc3=$?"
complete -p missing-command 2>/dev/null
echo "missing-name=$?"
complete -p -D 2>/dev/null
echo "missing-default=$?"
