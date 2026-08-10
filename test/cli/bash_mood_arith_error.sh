unset KOSH_FLAGS
echo "== bash mood reports an arithmetic error and goes on to the next line, matching bash:"
"$BIN" --mood bash -c 'echo before
echo $((1/0))
echo after'
echo "rc=$?"
echo "== the strict default mood aborts the run on an arithmetic error:"
"$BIN" -c 'echo $((1/0)); echo after'
echo "rc=$?"
echo "== bash mood still continues past an ordinary command failure:"
"$BIN" --mood bash -c 'false; echo after'
echo "rc=$?"
echo "== bash mood still continues past a readonly reassignment, matching bash:"
"$BIN" --mood bash -c 'readonly r=1; r=2; echo after'
echo "rc=$?"
