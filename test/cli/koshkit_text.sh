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
"$BIN" -c 'koshkit seq 20000' > batch-input.txt
: > empty.txt
printf 'first\n' > cat-first.txt
printf 'last\n' > cat-last.txt
printf 'abc' > transform-first.txt
printf '\tx\n' > transform-tabs.txt
printf '        x\n' > transform-blanks.txt
printf 'def\n' > transform-second.txt
for batch_source_index in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18; do
  printf 'source-%s\n' "$batch_source_index" > "batch-source-$batch_source_index.txt"
  printf '1\n' > "bc-source-$batch_source_index.txt"
done
echo "--- cat multi-chunk input with a missing operand ---"
"$BIN" -c \
  'koshkit cat batch-input.txt cat-first.txt missing.txt cat-last.txt > cat-output.txt; printf "status=%s\n" "$?"; koshkit cksum cat-output.txt' \
  2>&1
echo "--- numbered cat reads later files after a missing operand ---"
"$BIN" -c \
  'koshkit cat -n cat-first.txt missing.txt cat-last.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- cat preserves bounded source-window order ---"
"$BIN" -c \
  'koshkit cat batch-source-18.txt batch-source-01.txt batch-source-17.txt batch-source-02.txt batch-source-16.txt batch-source-03.txt batch-source-15.txt batch-source-04.txt batch-source-14.txt batch-source-05.txt batch-source-13.txt batch-source-06.txt batch-source-12.txt batch-source-07.txt batch-source-11.txt batch-source-08.txt batch-source-10.txt batch-source-09.txt'
echo "--- transformed sources preserve boundaries ---"
"$BIN" -c 'koshkit expand -t 4 transform-first.txt transform-tabs.txt'
"$BIN" -c 'koshkit unexpand transform-first.txt transform-blanks.txt' | cat -v
"$BIN" -c 'koshkit fold -w 4 transform-first.txt transform-second.txt'
echo "--- transformed utilities batch source windows ---"
"$BIN" -c 'koshkit expand batch-source-*.txt | koshkit cksum'
"$BIN" -c 'koshkit unexpand batch-source-*.txt | koshkit cksum'
"$BIN" -c 'koshkit fold batch-source-*.txt | koshkit cksum'
"$BIN" -c 'koshkit cat -n batch-source-*.txt | koshkit cksum'
echo "--- od and bc batch source windows ---"
"$BIN" -c 'koshkit od -An -tc batch-source-*.txt | koshkit cksum'
"$BIN" -c 'koshkit bc bc-source-*.txt | koshkit cksum'
echo "--- cat reads standard input between files ---"
printf 'middle\n' | "$BIN" -c \
  'koshkit cat cat-first.txt - cat-last.txt'
"$BIN" -c "koshkit yes x | koshkit tr -d '\n' | koshkit head -c 65534" \
  > strings-boundary.txt
printf 'tail\0' >> strings-boundary.txt
echo "--- strings preserves a run across chunks ---"
"$BIN" -c \
  'koshkit strings -t d strings-boundary.txt | koshkit wc -c; koshkit strings strings-boundary.txt | koshkit tail -c 5'
echo "--- strings reads later files after a missing operand ---"
"$BIN" -c \
  'koshkit strings cat-first.txt missing.txt cat-last.txt; printf "status=%s\n" "$?"' \
  2>&1
"$BIN" -c "koshkit yes x | koshkit tr -d '\n' | koshkit head -c 65535" \
  > cut-boundary.txt
printf ':tail\n' >> cut-boundary.txt
echo "--- cut preserves a line across chunks ---"
"$BIN" -c 'koshkit cut -d : -f 2 cut-boundary.txt'
"$BIN" -c "koshkit yes x | koshkit tr -d '\n' | koshkit head -c 65535" \
  > cut-utf8-boundary.txt
printf '\303\251z' >> cut-utf8-boundary.txt
echo "--- cut preserves split UTF-8 at final line ---"
"$BIN" -c \
  'koshkit cut -c 65536-65537 cut-utf8-boundary.txt | koshkit od -An -tx1; koshkit cut -b 65537 -n cut-utf8-boundary.txt | koshkit od -An -tx1'
echo "--- cut reads later files after a missing operand ---"
"$BIN" -c \
  'koshkit cut -c 1-5 cat-first.txt missing.txt cat-last.txt; printf "status=%s\n" "$?"' \
  2>&1
"$BIN" -c 'koshkit seq 19999; printf "20001\n"' > diff-right.txt
echo "--- diff reads two multi-chunk files ---"
"$BIN" -c \
  'koshkit diff batch-input.txt diff-right.txt; printf "status=%s\n" "$?"'
echo "--- diff reads standard input on the right ---"
printf 'first\n' | "$BIN" -c \
  'koshkit diff cat-first.txt -; printf "status=%s\n" "$?"'
echo "--- diff reports a missing second operand ---"
"$BIN" -c \
  'koshkit diff batch-input.txt missing.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- wc multi-chunk input with a missing operand ---"
"$BIN" -c \
  'koshkit wc -c batch-input.txt missing.txt empty.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- wc repeated explicit standard input ---"
printf 'abc\n' | "$BIN" -c 'koshkit wc -c - -'
echo "--- wc implicit standard input ---"
printf 'abc\n' | "$BIN" -c 'koshkit wc -c'
echo "--- cksum multi-chunk input with a missing operand ---"
"$BIN" -c \
  'koshkit cksum batch-input.txt missing.txt empty.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- cksum repeated explicit standard input ---"
printf 'abc\n' | "$BIN" -c 'koshkit cksum - -'
echo "--- cksum implicit standard input ---"
printf 'abc\n' | "$BIN" -c 'koshkit cksum'
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
printf 'zulu\nalpha\n' > sort-a.txt
printf 'middle\nbeta\n' > sort-b.txt
echo "--- sort multiple files with a missing operand ---"
"$BIN" -c \
  'koshkit sort sort-a.txt missing.txt sort-b.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- sort repeated standard input ---"
printf 'delta\nalpha\n' | "$BIN" -c 'koshkit sort - -'
echo "--- paste multiple files with a missing operand ---"
"$BIN" -c \
  'koshkit paste -d , sort-a.txt missing.txt sort-b.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- paste repeated standard input ---"
printf 'left\nright\n' | "$BIN" -c 'koshkit paste -d , - -'
echo "--- pr merge with a missing operand ---"
"$BIN" -c \
  'koshkit pr -t -m -s , sort-a.txt missing.txt sort-b.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- pr merge suppresses a missing warning ---"
"$BIN" -c \
  'koshkit pr -r -t -m -s , sort-a.txt missing.txt sort-b.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- pr batches normal source windows ---"
"$BIN" -c 'koshkit pr -t batch-source-*.txt | koshkit cksum'
echo "--- pr numbering resets for each source ---"
"$BIN" -c 'koshkit pr -t -n cat-first.txt cat-last.txt'
echo "--- pr reads later files after a missing operand ---"
"$BIN" -c \
  'koshkit pr -t sort-a.txt missing.txt sort-b.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- pr normal mode suppresses a missing warning ---"
"$BIN" -c \
  'koshkit pr -r -t sort-a.txt missing.txt sort-b.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- grep an ---"
"$BIN" -c 'koshkit grep an fruit.txt'
echo "--- grep -v apple ---"
"$BIN" -c 'koshkit grep -v apple fruit.txt'
echo "--- grep -i APPLE ---"
"$BIN" -c 'koshkit grep -i APPLE fruit.txt'
echo "--- grep stdin ---"
printf 'pear\nplum\n' | "$BIN" -c 'koshkit grep plum'
echo "--- grep unterminated final line ---"
printf 'tail' | "$BIN" -c 'koshkit grep tail'
printf 'pear\n' > pear.txt
echo "--- grep multiple files ---"
"$BIN" -c 'koshkit grep pear fruit.txt pear.txt'
"$BIN" -c "koshkit yes x | koshkit tr -d '\n' | koshkit head -c 65534" \
  > grep-boundary.txt
printf 'needle\n' >> grep-boundary.txt
echo "--- grep match across chunk boundary ---"
"$BIN" -c 'koshkit grep needle grep-boundary.txt | koshkit wc -c'
echo "--- grep multiple files with a missing operand ---"
"$BIN" -c \
  'koshkit grep a sort-a.txt missing.txt sort-b.txt; printf "status=%s\n" "$?"' \
  2>&1
echo "--- tr to lower ---"
"$BIN" -c 'printf "AbC\n" | koshkit tr A-Z a-z'
echo "--- tr -d digits ---"
"$BIN" -c 'printf "a1b2c3\n" | koshkit tr -d 0-9'
echo "--- tr reverse range ---"
printf "abc\n" | "$BIN" -c 'koshkit tr a-c z-x'
echo "--- seq into head ---"
"$BIN" -c 'koshkit seq 5 | koshkit head -n 2'
echo "--- head minimum signed drop count ---"
printf 'one\ntwo\n' | "$BIN" -c 'koshkit head -n -9223372036854775808'
echo "status=$?"
echo "--- tee then read back ---"
"$BIN" -c 'koshkit seq 2 | koshkit tee tee.txt'
"$BIN" -c 'koshkit cat tee.txt'
echo "--- tee copies to multiple files and appends ---"
printf 'old\n' > tee-first.txt
printf 'one\ntwo\n' | "$BIN" -c \
  'koshkit tee -a tee-first.txt tee-second.txt'
echo "first:"
"$BIN" -c 'koshkit cat tee-first.txt'
echo "second:"
"$BIN" -c 'koshkit cat tee-second.txt'
echo "--- empty tee input truncates outputs ---"
printf 'old\n' > tee-empty.txt
printf '' | "$BIN" -c 'koshkit tee tee-empty.txt'
"$BIN" -c 'koshkit wc -c < tee-empty.txt'
echo "--- tee copies more than one chunk ---"
"$BIN" -c \
  'koshkit seq 20000 | koshkit tee tee-large-1.txt tee-large-2.txt tee-large-3.txt tee-large-4.txt tee-large-5.txt tee-large-6.txt tee-large-7.txt tee-large-8.txt | koshkit wc -l'
"$BIN" -c 'koshkit cmp tee-large-1.txt tee-large-8.txt'
echo "same=$?"
echo "--- tee keeps working outputs after an open failure ---"
mkdir tee-directory
printf 'kept\n' | "$BIN" -c \
  'koshkit tee tee-working.txt tee-directory 2>/dev/null; printf "status=%s\n" "$?"'
"$BIN" -c 'koshkit cat tee-working.txt'
echo "--- seq with step ---"
"$BIN" -c 'koshkit seq 2 2 8'
