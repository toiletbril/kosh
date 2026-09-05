#!/bin/bash
# Alternative for bash_variables.bash on a bash older than 5.3, which lacks
# BASH_MONOSECONDS, and on a distro or platform whose bash reports a HOSTTYPE
# and a MACHTYPE target other than kosh's. The monoseconds line is
# unconditional, and the platform values are derived from the running system.
[ "$SECONDS" -eq 0 ] && echo "seconds starts zero"
r=$RANDOM
[ "$r" -ge 0 ] && [ "$r" -le 32767 ] && echo "random in range"
a=$RANDOM
b=$RANDOM
c=$RANDOM
[ "$a$b$c" != "000" ] && echo "random produces values"
[ "$BASHPID" -gt 0 ] && echo "bashpid positive"
[ "$EPOCHSECONDS" -gt 1000000000 ] && echo "epoch is a unix time"
[ "$UID" -ge 0 ] && echo "uid present"
[ "$EUID" -ge 0 ] && echo "euid present"
[ "$PPID" -gt 0 ] && echo "ppid positive"
echo "monoseconds positive"
machine=$(uname -m)
system=$(uname -s)
if [ "$system" = Darwin ] && [ "$machine" = arm64 ]; then
  machine=aarch64
fi
echo "hosttype=$machine"
if [ "$system" = Darwin ]; then
  echo "machtype=$machine-apple-darwin$(uname -r)"
else
  echo "machtype=$machine-unknown-linux-gnu"
fi
[ "$GROUPS" -ge 0 ] && echo "groups present"
[ -n "$SRANDOM" ] && echo "srandom present"
