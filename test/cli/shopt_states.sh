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
sourcepath_directory=$TEST_TEMP_DIRECTORY/sourcepath
sourcepath_working_directory=$TEST_TEMP_DIRECTORY/sourcepath-working
mkdir -p "$sourcepath_directory"
mkdir -p "$sourcepath_working_directory"
sourcepath_directory=$(cd "$sourcepath_directory" && pwd)
sourcepath_working_directory=$(cd "$sourcepath_working_directory" && pwd)
printf 'printf "path\\n"\n' > "$sourcepath_directory/sourcepath-probe"
printf 'printf "local\\n"\n' > "$sourcepath_working_directory/sourcepath-probe"
echo "== sourcepath controls PATH lookup:"
"$BIN" --mood bash --no-init-files --no-diagnostics -c '
cd "$2"
PATH=$1:$PATH
source sourcepath-probe
printf "on=%s\n" "$?"
shopt -u sourcepath
source sourcepath-probe
printf "off=%s\n" "$?"
' kosh "$sourcepath_directory" "$sourcepath_working_directory"
echo "== inherit_errexit controls command substitutions:"
"$BIN" --mood bash --no-init-files --no-diagnostics -c '
set -e
shopt -u inherit_errexit
value=$(false; printf kept)
printf "off=%s:%s\n" "$?" "$value"
'
echo "off-rc=$?"
"$BIN" --mood bash --no-init-files --no-diagnostics -c '
set -e
shopt -s inherit_errexit
value=$(false; printf lost)
printf "on=%s:%s\n" "$?" "$value"
'
echo "on-rc=$?"
echo "== lastpipe controls the final builtin:"
"$BIN" --mood bash --no-init-files --no-diagnostics -c '
unset value
printf x | read value
printf "default=<%s>\n" "$value"
shopt -s lastpipe
set +m
printf y | read value
printf "lastpipe=<%s>\n" "$value"
set -m
printf z | read value
printf "monitor=<%s>\n" "$value"
set +m
printf group | { read value; }
printf "group=<%s>\n" "$value"
read_value() { read value; }
printf function | read_value
printf "function=<%s>\n" "$value"
'
echo "== localvar_inherit controls bare locals:"
"$BIN" --mood bash --no-init-files --no-diagnostics -c '
x=outer
f()
{
  local x
  printf "value=<%s> set=%s\n" "$x" "${x+x}"
}
f
shopt -s localvar_inherit
f
declare -i integer=7
integer_test()
{
  local integer
  integer=2+3
  printf "integer=%s\n" "$integer"
}
integer_test
export exported=outer
export_test()
{
  local exported
  declare -p exported
}
shopt -u localvar_inherit
export_test
values=(one two)
array_test()
{
  local values
  printf "array=%s\n" "${values[*]}"
}
shopt -s localvar_inherit
array_test
'
checkhash_first=$TEST_TEMP_DIRECTORY/checkhash-first
checkhash_second=$TEST_TEMP_DIRECTORY/checkhash-second
mkdir -p "$checkhash_first" "$checkhash_second"
printf '#!/bin/sh\nprintf "first\\n"\n/bin/mv "$0" "$0.off"\n' > "$checkhash_first/checkhash-probe"
printf '#!/bin/sh\nprintf "second\\n"\n' > "$checkhash_second/checkhash-probe"
printf '#!/bin/sh\nprintf "rehash first\\n"\n' > "$checkhash_first/rehash-probe"
printf '#!/bin/sh\nprintf "rehash second\\n"\n' > "$checkhash_second/rehash-probe.off"
chmod +x "$checkhash_first/checkhash-probe" "$checkhash_second/checkhash-probe" \
  "$checkhash_first/rehash-probe" "$checkhash_second/rehash-probe.off"
echo "== checkhash controls cached command validation:"
"$BIN" --mood bash --no-init-files --no-diagnostics -c '
PATH=$1:$2
checkhash-probe
checkhash-probe
printf "off=%s\n" "$?"
shopt -s checkhash
checkhash-probe
printf "on=%s\n" "$?"
hash -R
printf "rehash=%s\n" "$?"
' kosh "$checkhash_first" "$checkhash_second"
echo "== hash -R rebuilds the PATH index:"
"$BIN" --mood bash --no-init-files --no-diagnostics -c '
PATH=$1:$2
rehash-probe
koshkit mv "$1/rehash-probe" "$1/rehash-probe.off"
koshkit mv "$2/rehash-probe.off" "$2/rehash-probe"
hash -R
rehash-probe
' kosh "$checkhash_first" "$checkhash_second"
