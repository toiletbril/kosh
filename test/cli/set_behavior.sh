unset KOSH_FLAGS
# set -e aborts the run on the first failing command, set +e turns it back off,
# an unknown --mood value is rejected, and set -o with no name lists the option
# states.
echo "== set -e aborts before the next command:"
"$BIN" -c 'set -e; false; echo unreached'; echo "rc=$?"
echo "== set +e lets the run continue:"
"$BIN" -c 'set -e; set +e; false; echo reached'; echo "rc=$?"
echo "== an unknown mood is rejected:"
"$BIN" -c 'set --mood badmood'; echo "rc=$?"
echo "== set -o lists the option states:"
"$BIN" -c 'set -o'

unset KOSH_FLAGS
# set --mood and its short -M switch the runtime mood and reseed the strictness,
# and -L is the short for --init-moods, matching the command-line flags so a
# config can set either form.
echo "== bash mood relaxes nounset:"
"$BIN" -c 'set --mood bash; echo "[${UNSETA}]"; echo ok'
echo "== -M short form prints the active mood:"
"$BIN" -c 'set -M sh; set --mood'
echo "== default mood is strict:"
"$BIN" -c 'echo "[${UNSETB}]"' 2>&1 | grep -o "is not set" | head -1
echo "== switching back to kosh restores strictness:"
"$BIN" -c 'set --mood bash; set --mood kosh; echo "[${UNSETC}]"' 2>&1 | grep -o "is not set" | head -1
echo "rc-done"

unset KOSH_FLAGS
# set -b backs a real notify option now, so the state round-trips through
# set +o and the letter form.
"$BIN" -c 'set -o | grep notify'
"$BIN" -c 'set -b; set -o | grep notify'
"$BIN" -c 'set -o notify; set +b; set -o | grep notify'
echo "rc=$?"

unset KOSH_FLAGS
# set -o posix mirrors set --mood sh, entering the POSIX mood. set +o posix
# steps down to bash only when already in POSIX, and is a no-op otherwise since
# the prior mood is not recoverable.
echo "== set -o posix enters posix mood:"
"$BIN" -c 'set -o posix; set -o | grep posix'
echo "== set +o posix from bash mood is a no-op:"
"$BIN" -M bash -c 'set +o posix; set -o | grep posix'
echo "== set +o posix from posix mood steps to bash:"
"$BIN" -M bash -c 'set -o posix; set +o posix; set -o | grep posix'
echo "== brew's two failing lines now pass:"
"$BIN" -M bash -c 'set +o posix; builtin enable compgen unset; echo ok'
echo "rc-done"
