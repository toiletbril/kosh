# The koshkit text utilities run against a fixed input file in a temporary
# directory, so the counts and the sorted output are the same everywhere.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1
printf 'banana\napple\ncherry\napple\n' > fruit.txt

echo "--- cat -n ---"
"$BIN" -c 'koshkit cat -n fruit.txt'
echo "--- wc ---"
"$BIN" -c 'koshkit wc fruit.txt'
echo "--- wc -l ---"
"$BIN" -c 'koshkit wc -l fruit.txt'
echo "--- head -n 2 ---"
"$BIN" -c 'koshkit head -n 2 fruit.txt'
echo "--- tail -n 1 ---"
"$BIN" -c 'koshkit tail -n 1 fruit.txt'
echo "--- sort ---"
"$BIN" -c 'koshkit sort fruit.txt'
echo "--- sort -r ---"
"$BIN" -c 'koshkit sort -r fruit.txt'
echo "--- sort then uniq -c ---"
"$BIN" -c 'koshkit sort fruit.txt | koshkit uniq -c'
echo "--- grep an ---"
"$BIN" -c 'koshkit grep an fruit.txt'
echo "--- grep -v apple ---"
"$BIN" -c 'koshkit grep -v apple fruit.txt'
echo "--- grep -i APPLE ---"
"$BIN" -c 'koshkit grep -i APPLE fruit.txt'
echo "--- grep stdin ---"
printf 'pear\nplum\n' | "$BIN" -c 'koshkit grep plum'
printf 'pear\n' > pear.txt
echo "--- grep multiple files ---"
"$BIN" -c 'koshkit grep pear fruit.txt pear.txt'
echo "--- tr to lower ---"
"$BIN" -c 'printf "AbC\n" | koshkit tr A-Z a-z'
echo "--- tr -d digits ---"
"$BIN" -c 'printf "a1b2c3\n" | koshkit tr -d 0-9'
echo "--- tr reverse range ---"
printf "abc\n" | "$BIN" -c 'koshkit tr a-c z-x'
echo "--- seq into head ---"
"$BIN" -c 'koshkit seq 5 | koshkit head -n 2'
echo "--- tee then read back ---"
"$BIN" -c 'koshkit seq 2 | koshkit tee tee.txt'
"$BIN" -c 'koshkit cat tee.txt'
echo "--- seq with step ---"
"$BIN" -c 'koshkit seq 2 2 8'
