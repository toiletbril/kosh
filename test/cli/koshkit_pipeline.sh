# A koshkit producer runs as a forked pipeline stage, so it streams to its
# consumer and ends on a broken pipe rather than blocking the shell. The default
# mood has pipefail on, so a producer ended by SIGPIPE makes the pipeline report
# 141, the way bash with pipefail does.
unset KOSH_FLAGS
NULL_DEVICE=${TEST_NULL_DEVICE:-/dev/null}

echo "--- yes into head ---"
"$BIN" -c 'koshkit yes hi | koshkit head -n 2'
echo "rc=$?"

echo "--- large seq into head ---"
"$BIN" -c 'koshkit seq 100000 | koshkit head -n 3'
echo "rc=$?"

echo "--- infinite input through cat into head ---"
"$BIN" -c 'koshkit yes cat | koshkit cat | koshkit head -n 1'
echo "rc=$?"

echo "--- infinite input through grep and tee into head ---"
"$BIN" -c "koshkit yes grep | koshkit grep grep | koshkit tee '$NULL_DEVICE' | koshkit head -n 1"
echo "rc=$?"

echo "--- seq through cat into wc counts every line ---"
"$BIN" -c 'koshkit seq 50 | koshkit cat | koshkit wc -l'
echo "rc=$?"

echo "--- seq into grep ---"
"$BIN" -c 'koshkit seq 12 | koshkit grep 1'

echo "--- sort into uniq ---"
"$BIN" -c 'printf "c\na\nb\na\n" | koshkit sort | koshkit uniq'
