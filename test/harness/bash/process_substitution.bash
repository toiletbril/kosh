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
cat <(
  printf 'comment-close\n' # )
  printf 'nested-close:%s\n' "$(printf ')')"
)
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

# A process substitution in a for word list stays open through the loop and
# closes when the loop finishes, so its path no longer opens afterwards.
for path in <(echo one) <(echo two); do
  cat "$path"
  last_path=$path
done
if cat "$last_path" 2>/dev/null; then echo "loop path open"; else
  echo "loop path closed"
fi

# A redirection of another descriptor leaves the substitution path readable.
cat <(echo beside-redirection) 3</dev/null

# A process substitution sets $! to its process, and wait reads its status
# after the command reaped it, also a second time. jobs does not list one, a
# later background job replaces $!, and wait with no operand forgets them.
# Bash may prune the status of an older one depending on when it reaps it,
# so process_substitution_wait.kosh covers that case.
cat <(exit 3)
wait "$!"
case $? in
  3 | 127) echo "input=kept-or-pruned" ;;
  *) echo "input=unexpected" ;;
esac
: > >(exit 4)
wait "$!"
echo "output=$?"
cat <(exit 6) <(exit 7)
wait "$!"
echo "last=$?"
wait "$!"
echo "again=$?"
assigned=<(exit 8)
wait "$!"
echo "assigned=$?"
for code in 1 2 9; do cat <(exit "$code"); done
wait "$!"
echo "loop=$?"
[[ -e <(exit 10) ]]
wait "$!"
echo "conditional=$?"
exec 3< <(sleep 0.2; exit 11)
jobs
wait "$!"
echo "held=$?"
exec 3<&-
cat <(exit 12)
substituted=$!
sleep 0 &
[ "$!" != "$substituted" ] && echo "background-replaces"
wait "$substituted"
echo "before-background=$?"
wait
echo "no-operand=$?"
wait "$substituted" 2>/dev/null
echo "forgotten=$?"
