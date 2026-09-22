# The find utility walks a fixed temporary tree, so the relative paths it prints
# stay the same on every machine. The children of a directory are listed in
# sorted order, so the whole walk is deterministic.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1

"$BIN" -c 'koshkit mkdir -p a/b/c'
"$BIN" -c 'koshkit touch a/one.txt'
"$BIN" -c 'koshkit touch a/MiXeD.TxT'
"$BIN" -c 'koshkit touch a/b/two.log'
"$BIN" -c 'koshkit touch a/b/c/three.txt'
"$BIN" -c 'koshkit ln -sf missing broken'
"$BIN" -c 'koshkit mkdir -p long/abcdefghijklmnopqrstuvwxyz/segment'
"$BIN" -c 'koshkit touch long/abcdefghijklmnopqrstuvwxyz/segment/leaf'
"$BIN" -c 'koshkit ln -s a/b/c/three.txt file-link'

echo "--- find all ---"
"$BIN" -c 'koshkit find .'
echo "--- find -name *.txt ---"
"$BIN" -c 'koshkit find . -name "*.txt"'
echo "--- find -iname *.TXT ---"
"$BIN" -c 'koshkit find . -iname "*.TXT"'
"$BIN" -c 'koshkit touch "a/literal[bracket].txt"'
echo "--- find escaped metacharacter ---"
"$BIN" -c 'koshkit find . -name "literal\[bracket\].txt"'
echo "--- find slash-containing no match ---"
"$BIN" -c 'koshkit find . -iname "a/*.TXT"; printf "status=%s\\n" "$?"'
echo "--- find no match ---"
"$BIN" -c 'koshkit find . -iname "*.does-not-exist"; printf "status=%s\\n" "$?"'
echo "--- find -type d ---"
"$BIN" -c 'koshkit find . -type d'
echo "--- find -maxdepth 1 ---"
"$BIN" -c 'koshkit find . -maxdepth 1'
echo "--- find -mindepth 3 -type f ---"
"$BIN" -c 'koshkit find . -mindepth 3 -type f'
echo "--- find a named root ---"
"$BIN" -c 'koshkit find a/b'
echo "--- find multiple roots ---"
"$BIN" -c 'koshkit find a/one.txt a/b -maxdepth 0'
echo "--- find a dangling symlink root ---"
"$BIN" -c 'koshkit find broken -type l -maxdepth 0'
echo "--- find an explicit file symlink root ---"
"$BIN" -c 'koshkit find file-link -type l -maxdepth 0'
echo "--- find a long path ---"
"$BIN" -c 'koshkit find long -name leaf'
echo "--- find unknown predicate ---"
"$BIN" -c 'koshkit find . -bogus' 2>&1
echo "--- find missing -name argument ---"
"$BIN" -c 'koshkit find . -name' 2>&1
echo "--- find invalid -type argument ---"
"$BIN" -c 'koshkit find . -type x' 2>&1
echo "--- find missing -type argument ---"
"$BIN" -c 'koshkit find . -type' 2>&1
echo "--- find negative -maxdepth argument ---"
"$BIN" -c 'koshkit find . -maxdepth -1' 2>&1
echo "--- find invalid -mindepth argument ---"
"$BIN" -c 'koshkit find . -mindepth many' 2>&1
echo "--- find missing -maxdepth argument ---"
"$BIN" -c 'koshkit find . -maxdepth' 2>&1
echo "--- find valid -maxdepth argument ---"
"$BIN" -c 'koshkit find . -maxdepth 0'
echo "--- find unreadable nested path ---"
mkdir -p a/private
: > a/private/entry
chmod 000 a/private
if ls a/private >/dev/null 2>&1; then
  chmod 700 a/private
  echo "find-unreadable=skipped"
else
  unreadable_output=$("$BIN" -c 'koshkit find a/private' 2>&1)
  unreadable_status=$?
  chmod 700 a/private
  case $unreadable_output in
    *entry*) echo "find-unreadable=failed" ;;
    *)
      if [ "$unreadable_status" -ne 0 ]; then
        echo "find-unreadable=matched"
      else
        echo "find-unreadable=failed"
      fi
      ;;
  esac
fi
echo "--- find interruption ---"
if [ "${TARGET-}" != Linux ] || ! command -v timeout >/dev/null 2>&1; then
  echo "find-interrupt=skipped"
else
  interrupt_fast_root=$TEST_TEMP_DIRECTORY/find-interrupt-fast
  interrupt_root=$TEST_TEMP_DIRECTORY/find-interrupt-slow
  mkdir -p "$interrupt_fast_root" "$interrupt_root"
  : > "$interrupt_fast_root/complete"
  interrupt_directory=0
  while [ "$interrupt_directory" -lt 200 ]; do
    interrupt_path=$interrupt_root/d$interrupt_directory
    mkdir "$interrupt_path"
    interrupt_file=0
    while [ "$interrupt_file" -lt 200 ]; do
      printf x > "$interrupt_path/f$interrupt_file"
      interrupt_file=$((interrupt_file + 1))
    done
    interrupt_directory=$((interrupt_directory + 1))
  done
  interrupt_output=$(timeout --preserve-status -s INT 0.005s "$BIN" -c \
    "koshkit find '$interrupt_fast_root' '$interrupt_root'" 2>&1)
  interrupt_status=$?
  if [ "$interrupt_status" -eq 130 ]; then
    echo "find-interrupt=matched"
  else
    echo "find-interrupt=failed"
  fi
fi
