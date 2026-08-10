#!/bin/bash
# Bash process substitution <(...) and >(...), checked byte-for-byte against
# bash. The input form runs the command on a pipe and substitutes the /dev/fd
# path the reader opens, the output form substitutes the path the writer feeds.
cat <(echo hello)
cat <(echo one) <(echo two)
wc -l < <(seq 5) | tr -d ' '
diff <(printf 'a\nb\nc\n') <(printf 'a\nx\nc\n') | grep -c '^[<>]'
echo <(true) | grep -c '^/dev/fd/'
sort <(printf '3\n1\n2\n')
echo data | tee >(cat > /tmp/kosh_ps_out_$$) >/dev/null
sleep 0.2
cat /tmp/kosh_ps_out_$$
rm -f /tmp/kosh_ps_out_$$


# A process substitution as a while loop's redirection target is read correctly
# across every iteration and the loop runs to completion, the common idiom
# done < <(cmd), and a later command still runs after the loop.
while read -r line; do
  echo "got:$line"
done < <(printf 'a\nb\nc\n')
echo after
total=0
while read -r n; do
  total=$((total + n))
done < <(printf '10\n20\n30\n')
echo "total=$total"
