unset KOSH_FLAGS
# shopt queries, sets, and unsets a bash shell option, the -q form reports the
# state through the status alone, the -p form prints a replayable command, an
# unknown name is an error or a silent non-zero under -q, and -o bridges to a
# set -o option name.
echo "== a default option queries off:"; "$BIN" -c 'shopt nullglob'; echo "rc=$?"
echo "== set then query:"; "$BIN" -c 'shopt -s nullglob; shopt nullglob'; echo "rc=$?"
echo "== unset then query:"; "$BIN" -c 'shopt -s nullglob; shopt -u nullglob; shopt nullglob'; echo "rc=$?"
echo "== -q is silent and reports off through the status:"; "$BIN" -c 'shopt -q nullglob'; echo "rc=$?"
echo "== -q reports on through the status:"; "$BIN" -c 'shopt -s nullglob; shopt -q nullglob'; echo "rc=$?"
echo "== -p prints the replayable form when off:"; "$BIN" -c 'shopt -p nullglob'
echo "== -p prints the replayable form when on:"; "$BIN" -c 'shopt -s nullglob; shopt -p nullglob'
echo "== an unknown name is an error:"; "$BIN" -c 'shopt bogusopt'; echo "rc=$?"
echo "== an unknown name under -q is silent:"; "$BIN" -c 'shopt -q bogusopt'; echo "rc=$?"
echo "== -o queries a set option name:"; "$BIN" -c 'shopt -o noexec'; echo "rc=$?"
