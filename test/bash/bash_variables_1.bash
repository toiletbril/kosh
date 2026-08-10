#!/bin/bash
# Alternative for bash_variables.bash on a bash older than 5.3, which lacks
# BASH_MONOSECONDS, and on a distro or platform whose bash reports a HOSTTYPE
# and a MACHTYPE vendor other than kosh's. The monoseconds line is unconditional,
# and the hosttype and the machtype are spelled to match kosh. kosh reports the
# uname machine as the hosttype, and the uname machine joined with the fixed
# vendor string "-unknown-linux-gnu" as the machtype, so the alt reads uname to
# match both.
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
echo "hosttype=$(uname -m)"
echo "machtype=$(uname -m)-unknown-linux-gnu"
[ "$GROUPS" -ge 0 ] && echo "groups present"
[ -n "$SRANDOM" ] && echo "srandom present"
