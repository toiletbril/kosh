#!/bin/bash
# Several output process substitutions on one command finish instead of
# each waiting for the descriptor another one holds, a pipeline works in a
# shell started with standard input closed, and a long chain of unary
# operators evaluates, all as in bash.
work=$(mktemp -d)
cd "$work" || exit 1
echo x | tee >(cat > first) >(cat > second) >(cat > third) > /dev/null
wait
sleep 0.2
echo "tee=$(cat first second third | tr '\n' ,)"
printf 'one\ntwo\n' | tee >(wc -l > count_a) >(wc -l > count_b) > /dev/null
sleep 0.2
echo "counts=$(tr -d ' ' < count_a) $(tr -d ' ' < count_b)"
printf '%s\n' 'echo y | cat' 'echo y | { read -r line; echo "got $line"; }' > closed.sh
"$BASH" closed.sh 0<&-
"$BASH" -c 'echo all-closed | cat > all-closed.txt' 0<&- 1>&- 2>&-
cat all-closed.txt
nots=""
for ((i = 0; i < 3000; i++)); do nots+="!"; done
echo "not-chain=$(( $nots 1 ))"
cd / && rm -rf -- "$work"
