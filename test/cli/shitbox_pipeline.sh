# A shitbox producer runs as a forked pipeline stage, so it streams to its
# consumer and ends on a broken pipe rather than blocking the shell. The default
# mood has pipefail on, so a producer ended by SIGPIPE makes the pipeline report
# 141, the way bash with pipefail does.
unset SHIT_FLAGS
NULL_DEVICE=${TEST_NULL_DEVICE:-/dev/null}

echo "--- yes into head ---"
"$BIN" -c 'shitbox yes hi | shitbox head -n 2'
echo "rc=$?"

echo "--- large seq into head ---"
"$BIN" -c 'shitbox seq 100000 | shitbox head -n 3'
echo "rc=$?"

echo "--- infinite input through cat into head ---"
"$BIN" -c 'shitbox yes cat | shitbox cat | shitbox head -n 1'
echo "rc=$?"

echo "--- infinite input through grep and tee into head ---"
"$BIN" -c "shitbox yes grep | shitbox grep grep | shitbox tee '$NULL_DEVICE' | shitbox head -n 1"
echo "rc=$?"

echo "--- seq through cat into wc counts every line ---"
"$BIN" -c 'shitbox seq 50 | shitbox cat | shitbox wc -l'
echo "rc=$?"

echo "--- seq into grep ---"
"$BIN" -c 'shitbox seq 12 | shitbox grep 1'

echo "--- sort into uniq ---"
"$BIN" -c 'printf "c\na\nb\na\n" | shitbox sort | shitbox uniq'
