# The koshkit file utilities run inside a fresh temporary directory so the output
# names no absolute path and stays the same on every machine. The binary path is
# resolved to an absolute one first, since the working directory changes.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1

"$BIN" -c 'koshkit mkdir -p a/b'
"$BIN" -c 'koshkit seq 3 > nums.txt'
echo "--- ls ---"
"$BIN" -c 'koshkit ls'
echo "--- ls a ---"
"$BIN" -c 'koshkit ls a'
"$BIN" -c 'koshkit cp nums.txt copy.txt'
"$BIN" -c 'koshkit mv copy.txt moved.txt'
"$BIN" -c 'koshkit touch stamp'
"$BIN" -c 'koshkit ln -s nums.txt sym'
echo "--- file batched operands ---"
"$BIN" -c 'koshkit file nums.txt stamp sym missing.txt' 2>&1
echo "file-status=$?"
echo "--- custom file samples preserve merged-stream operand order ---"
mkdir .magic-batch
(
  cd .magic-batch || exit 1
  printf '%s\n' '0 string MATCH batch-match' > magic-batch
  printf MATCH > magic-good-a
  printf MATCH > magic-good-b
  "$BIN" -c \
    'koshkit file -M magic-batch magic-good-a missing-magic magic-good-b' \
    2>&1
  echo "custom-file-status=$?"
)
echo "--- stat preserves nonadjacent repeated operands ---"
"$BIN" -c 'koshkit stat -c "%n=%s" nums.txt stamp nums.txt'
mkdir batch-a batch-b
printf one > batch-a/first
printf two-two > batch-a/second
printf three > batch-b/third
printf four-four > batch-b/fourth
echo "--- stat preserves interleaved directory operands ---"
"$BIN" -c 'koshkit stat -c "%n=%s" batch-a/first batch-b/third batch-a/second batch-b/fourth'
# The owner, the group, and the time of a long row vary by machine, so only the
# mode, the link count, the size, and the name are kept for a stable golden.
echo "--- ls -l sym (mode nlink size name) ---"
# Linux always reports a symlink as mode 0777, while macOS stores the real
# symlink permissions, so the meaningless symlink mode field is folded to the
# Linux form before the row is trimmed.
"$BIN" -c 'koshkit ls -l sym' | sed 's/^l[rwxsStT-]\{9\}/lrwxrwxrwx/; s/^\([^[:space:]]*\)[[:space:]][[:space:]]*\([^[:space:]]*\)[[:space:]][[:space:]]*[^[:space:]]*[[:space:]][[:space:]]*[^[:space:]]*[[:space:]][[:space:]]*\([^[:space:]]*\).*[^[:space:]][[:space:]][[:space:]]*\([^[:space:]]*\)$/\1 \2 \3 \4/'
echo "--- ls after operations ---"
"$BIN" -c 'koshkit ls'
printf 'PK\007\010' > zip-span
printf '\067\177\006\203' > sqlite-wal
printf 'CDF\002' > netcdf-2
printf 'CDF\005' > netcdf-5
printf '\324\303\262\241' > pcap-le
printf '\241\262\074\115' > pcap-ns-be
printf '\115\074\262\241' > pcap-ns-le
echo "--- file signature variants ---"
"$BIN" -c 'koshkit file zip-span sqlite-wal netcdf-2 netcdf-5 pcap-le pcap-ns-be pcap-ns-le'
printf 'old\n' > cp-target.txt
printf 'new\n' > cp-source.txt
printf 'n\n' | "$BIN" -c 'koshkit cp -i cp-source.txt cp-target.txt' 2>/dev/null
printf 'cp-interactive-no=%s\n' "$(cat cp-target.txt)"
printf 'y\n' | "$BIN" -c 'koshkit cp -i cp-source.txt cp-target.txt' 2>/dev/null
printf 'cp-interactive-yes=%s\n' "$(cat cp-target.txt)"
chmod 640 cp-source.txt
"$BIN" -c 'koshkit cp -p cp-source.txt cp-preserved.txt'
cp_preserve_mode=$("$BIN" -c 'koshkit ls -l cp-preserved.txt' | cut -d ' ' -f 1)
if [ "${OS-}" = Windows_NT ]; then
  expected_cp_preserve_mode=-rw-rw-rw-
else
  expected_cp_preserve_mode=-rw-r-----
fi
if [ "$cp_preserve_mode" = "$expected_cp_preserve_mode" ]; then
  echo cp-preserve-mode=passed
else
  echo "cp-preserve-mode=failed [$cp_preserve_mode]"
fi
printf 'same-file\n' > same.txt
"$BIN" -c 'koshkit cp same.txt same.txt' 2>/dev/null
printf 'cp-same-status=%s contents=%s\n' "$?" "$(cat same.txt)"
ln same.txt same-link.txt
"$BIN" -c 'koshkit cp same.txt same-link.txt' 2>/dev/null
printf 'cp-hardlink-status=%s contents=%s\n' "$?" "$(cat same.txt)"
printf 'old\n' > mv-target.txt
printf 'new\n' > mv-source.txt
printf 'n\n' | "$BIN" -c 'koshkit mv -i mv-source.txt mv-target.txt' 2>/dev/null
printf 'mv-interactive-no=%s source=%s\n' "$(cat mv-target.txt)" "$([ -e mv-source.txt ] && echo present || echo missing)"
printf 'y\n' | "$BIN" -c 'koshkit mv -i mv-source.txt mv-target.txt' 2>/dev/null
printf 'mv-interactive-yes=%s source=%s\n' "$(cat mv-target.txt)" "$([ -e mv-source.txt ] && echo present || echo missing)"
echo "--- du -s nums.txt ---"
du_file_output=$("$BIN" -c 'koshkit du -s nums.txt')
set -- $du_file_output
du_file_blocks=$("$BIN" -c 'koshkit stat -c %b nums.txt')
if [ "$1" -eq "$((du_file_blocks * 512))" ]; then
  echo "du-file-allocation=matched"
else
  echo "du-file-allocation=wrong"
fi
if [ "${TARGET-}" = Linux ]; then
  dd if=/dev/zero of=du-sparse bs=1 count=0 seek=1048576 2>/dev/null
  du_sparse_output=$("$BIN" -c 'koshkit du -s du-sparse')
  set -- $du_sparse_output
  du_sparse_blocks=$("$BIN" -c 'koshkit stat -c %b du-sparse')
  if [ "$1" -eq "$((du_sparse_blocks * 512))" ]; then
    echo "du-sparse-allocation=matched"
  else
    echo "du-sparse-allocation=wrong"
  fi
else
  echo "du-sparse-allocation=skipped"
fi
mkdir -p du-default/sub
printf a > du-default/a
printf bb > du-default/b
printf 1234567890 > du-default/long-name
printf ccc > du-default/sub/c
ln -s sub du-default/sub-link
printf x > du-default/tie-a
printf y > du-default/tie-b
echo "--- du with no operand lists every entry and the total ---"
du_report=$d/du-default.out
(cd du-default && "$BIN" -c 'koshkit du') > "$du_report"
du_order=descending
du_path_mask=0
previous_size=
while read -r current_size current_path; do
  if [ -n "$previous_size" ] && [ "$current_size" -gt "$previous_size" ]; then
    du_order=wrong
  fi
  previous_size=$current_size
  case $current_path in
    .) du_path_mask=$((du_path_mask | 1)) ;;
    ./a) du_path_mask=$((du_path_mask | 2)) ;;
    ./b) du_path_mask=$((du_path_mask | 4)) ;;
    ./long-name) du_path_mask=$((du_path_mask | 8)) ;;
    ./sub) du_path_mask=$((du_path_mask | 16)) ;;
    ./sub/c) du_path_mask=$((du_path_mask | 32)) ;;
    ./sub-link) du_path_mask=$((du_path_mask | 64)) ;;
    *) du_path_mask=-1 ;;
  esac
done < "$du_report"
printf 'du-order=%s\n' "$du_order"
if [ "$du_path_mask" -eq 127 ]; then
  echo "du-recursion=complete"
else
  echo "du-recursion=incomplete"
fi
echo "--- du multiple roots ---"
"$BIN" -c 'koshkit du -s du-default/a du-default/b'
echo "--- du equal-size roots ---"
"$BIN" -c 'koshkit du -s du-default/tie-a du-default/tie-b'
mkdir du-links
printf x > du-links/a
ln du-links/a du-links/b
du_links_report=$d/du-links.out
"$BIN" -c 'koshkit du -s du-links/a du-links/b' > "$du_links_report"
du_link_line_count=$(wc -l < "$du_links_report")
if [ "$du_link_line_count" -eq 1 ]; then
  echo "du-hardlinks=deduplicated"
else
  echo "du-hardlinks=repeated"
fi
mkdir unreadable
touch unreadable/entry
chmod 000 unreadable
if ls unreadable >/dev/null 2>&1; then
  chmod 700 unreadable
  echo "du-unreadable=ok"
else
  unreadable_output=$("$BIN" -c 'koshkit du unreadable' 2>&1)
  unreadable_status=$?
  chmod 700 unreadable
  if [ "$unreadable_status" -ne 0 ] &&
    ! printf '%s\n' "$unreadable_output" | grep -q '^0[[:space:]]'; then
    echo "du-unreadable=ok"
  else
    echo "du-unreadable=failed"
  fi
fi
echo "--- basename ---"
"$BIN" -c 'koshkit basename /usr/local/libfoo.so .so'
echo "--- dirname ---"
"$BIN" -c 'koshkit dirname /usr/local/libfoo.so'
echo "--- realpath lexical basename ---"
"$BIN" -c 'koshkit mkdir -p usr/lib; resolved=$(koshkit realpath ./usr/../usr/lib) || exit; koshkit basename "$resolved"'
"$BIN" -c 'koshkit rmdir a/b'
echo "--- ls a after rmdir ---"
"$BIN" -c 'koshkit ls a'
"$BIN" -c 'koshkit mkdir -p parent/child; koshkit rmdir -p parent/child'
printf 'rmdir-parents=%s\n' "$([ -e parent ] && echo present || echo missing)"
echo "--- unlink removes a single file ---"
"$BIN" -c 'koshkit touch victim; koshkit unlink victim; echo "unlink rc=$?"'
"$BIN" -c 'koshkit ls' | grep -c victim
echo "--- unlink a directory fails ---"
"$BIN" -c 'koshkit unlink a' 2>&1
echo "rc=$?"
