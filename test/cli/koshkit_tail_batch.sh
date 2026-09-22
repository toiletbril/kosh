# Tail keeps exact POSIX count semantics while batching regular-file metadata
# and bounded positioned reads. The source order remains the operand order.
unset KOSH_FLAGS
d=$(mktemp -d)
normalized_d=$(printf '%s\n' "$d" | tr '\\' '/')
printf 'a\nb\nc\n' > "$d/with-final.txt"
printf 'a\nb\nc' > "$d/no-final.txt"
: > "$d/empty.txt"
printf '0123456789' > "$d/bytes.txt"
awk 'BEGIN { for (i = 1; i <= 70000; i++) print "x" }' \
  > "$d/large-forward.txt"
for batch_source_index in 01 02 03 04 05 06 07 08 09 10 11 12 13 14 15 16 17 18; do
  printf 'source-%s\nfirst-%s\nlast-%s\n' \
    "$batch_source_index" "$batch_source_index" "$batch_source_index" \
    > "$d/batch-$batch_source_index.txt"
done

echo "--- line and byte boundaries ---"
echo "tail -n 2 without final newline:"
"$BIN" -c "koshkit tail -n 2 '$d/no-final.txt'"
printf '\n'
echo "tail -n 0 empty file:"
"$BIN" -c "koshkit tail -n 0 '$d/empty.txt'"
printf '\n'
echo "tail -c 4:"
"$BIN" -c "koshkit tail -c 4 '$d/bytes.txt'"
printf '\n'
echo "--- positive offsets and standard input ---"
echo "tail -n +2:"
"$BIN" -c "koshkit tail -n +2 '$d/with-final.txt'"
printf '\n'
echo "tail -c +4:"
"$BIN" -c "koshkit tail -c +4 '$d/bytes.txt'"
printf '\n'
echo "tail from standard input:"
printf 'a\nb\nc\n' | "$BIN" -c 'koshkit tail -n 2'
printf '\n'
echo "--- forward batch boundary ---"
echo "tail -n +65538 line count:"
"$BIN" -c "koshkit tail -n +65538 '$d/large-forward.txt' | koshkit wc -l"
echo "tail -c +65537 byte count:"
"$BIN" -c "koshkit tail -c +65537 '$d/large-forward.txt' | koshkit wc -c"
printf '\n'
echo "--- bounded source order ---"
"$BIN" -c "koshkit tail -n 1 '$d/batch-01.txt' '$d/batch-02.txt' '$d/batch-03.txt' '$d/batch-04.txt' '$d/batch-05.txt' '$d/batch-06.txt' '$d/batch-07.txt' '$d/batch-08.txt' '$d/batch-09.txt' '$d/batch-10.txt' '$d/batch-11.txt' '$d/batch-12.txt' '$d/batch-13.txt' '$d/batch-14.txt' '$d/batch-15.txt' '$d/batch-16.txt' '$d/batch-17.txt' '$d/batch-18.txt'" \
  | tr '\\' '/' | sed "s#$normalized_d#TMPDIR#g"
echo "--- missing source preserves later output and status ---"
{
  cd "$d" || exit 1
  "$BIN" -c 'koshkit tail -n 1 missing.txt with-final.txt'
  printf 'status=%s\n' "$?"
} 2>&1 | tr '\\' '/' | sed "s#$normalized_d#TMPDIR#g"

if [ -n "$d" ]; then
  "$TEST_SYSTEM_RM" -r "$d"
fi
