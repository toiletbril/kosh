# Custom magic sampling keeps ordered results, offset-boundary behavior, and
# nonregular-input behavior while using bounded source batches.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
trap '[ -n "$d" ] && "$TEST_SYSTEM_RM" -r "$d"' EXIT
cd "$d" || exit 1

if [ "${OS-}" = Windows_NT ]; then
  echo 'platform=skipped'
  exit 0
fi

printf '%s\n' '0 string MATCH match' > magic
printf '%s\n' \
  '65534 string C edge-65535' \
  '65535 string A edge-65536' \
  '65536 string B edge-65537' > magic-boundaries
printf '%s\n' '0 string M parent' '>70000 string Z child' > magic-continuation
printf MATCH > match

echo '--- ordered errors ---'
printf MATCH > ordered-first
printf MATCH > ordered-last
{
  "$BIN" -c \
    'koshkit file -M magic ordered-first missing-magic ordered-last'
  printf 'status=%s\n' "$?"
} 2>&1

echo '--- exact boundaries ---'
"$BIN" -c 'koshkit yes X' | "$BIN" -c 'koshkit tr -d "\\n"' | "$BIN" -c \
  'koshkit head -c 65535' > edge-65536
printf A >> edge-65536
"$BIN" -c 'koshkit yes X' | "$BIN" -c 'koshkit tr -d "\\n"' | "$BIN" -c \
  'koshkit head -c 65536' > edge-65537
printf B >> edge-65537
"$BIN" -c 'koshkit yes X' | "$BIN" -c 'koshkit tr -d "\\n"' | "$BIN" -c \
  'koshkit head -c 65534' > edge-65535
printf C >> edge-65535
"$BIN" -c 'koshkit file -M magic-boundaries edge-65535 edge-65536 edge-65537'

printf M > continuation
"$BIN" -c 'koshkit yes X' | "$BIN" -c 'koshkit tr -d "\\n"' | "$BIN" -c \
  'koshkit head -c 69999' >> continuation
printf Z >> continuation
echo '--- far continuation ---'
"$BIN" -c 'koshkit file -M magic-continuation continuation'

echo '--- symlink precedence ---'
"$BIN" -c 'koshkit ln -s edge-65536 link'
"$BIN" -c 'koshkit file -h -L -M magic-boundaries link'
"$BIN" -c 'koshkit file -L -h -M magic-boundaries link'

echo '--- nonregular and literal dash ---'
"$BIN" -c 'koshkit mkfifo fifo'
"$BIN" -c 'koshkit file -M magic fifo'
printf MATCH > -
"$BIN" -c 'koshkit file -M magic -- -'

if [ "${OS-}" != Windows_NT ] && command -v truncate >/dev/null 2>&1 &&
  command -v dd >/dev/null 2>&1; then
  echo '--- sparse logical size ---'
  if truncate -s 1048576 sparse &&
    printf Z | dd of=sparse bs=1 seek=1048575 conv=notrunc \
      2> "$TEST_NULL_DEVICE"; then
    printf '%s\n' '1048575 string Z sparse-end' > sparse-magic
    "$BIN" -c 'koshkit file -M sparse-magic sparse'
  else
    echo 'sparse=setup-failed'
  fi
else
  echo '--- sparse logical size ---'
  echo 'sparse=skipped'
fi
