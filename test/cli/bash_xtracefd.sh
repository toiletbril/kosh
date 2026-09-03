log=$(mktemp)
echo "== trace steers to the descriptor, stdout still shows:"
"$BIN" --mood bash -c "exec 7>'$log'; BASH_XTRACEFD=7; set -x; echo hi; set +x; exec 7>&-" 2>&1
echo "== captured at the descriptor:"
cat "$log"
rm -f "$log"
echo "== without BASH_XTRACEFD the trace stays on stderr:"
"$BIN" --mood bash -c 'set -x; echo hi; set +x' 2>&1 1>/dev/null
echo "== an unparsable BASH_XTRACEFD falls back to stderr:"
"$BIN" --mood bash -c 'BASH_XTRACEFD=nope; set -x; echo hi; set +x' 2>&1 1>/dev/null
echo "== assignments, arrays, loops, and redirections trace by syntax owner:"
"$BIN" --mood bash -c 'set -x; scalar=value; scalar+=tail; arr=(one "two three"); empty=(); arr+=(four); PFX=value printf "%s\n" ok; for i in a b; do : "$i"; done; printf done >/dev/null; set +x' 2>&1
echo "== BlueBomb dependency arrays stay intact:"
"$BIN" --mood bash -c 'set -x; deps=(unzip wget); empty=(); managers=(pacman apt-get apt brew); set +x' 2>&1
