dir=$(mktemp -d) || exit 1
trap 'cd / && [ -n "$dir" ] && "$BIN_DIR/invoke-koshkit" rm -rf -- "$dir"' EXIT
cd "$dir" || exit 1

# ln -f refuses to replace a file with itself instead of deleting it, cp -r
# refuses a destination reached through a symlink into the source, tr fills
# [c*] and [c*n] repeats, paste deals stdin lines across repeated -, sed puts
# appended text on its own line after a last line without a newline, and a
# huge sort or uniq field count ends at the line end instead of spinning.
# fold keeps a missing final newline missing, and od refuses to skip past
# the end of its input.

kk() { "$BIN_DIR/invoke-koshkit" "$@"; }

echo data > same
kk ln -f same same 2> /dev/null
printf 'ln-same=%s content=%s\n' "$?" "$(cat same)"
mkdir sub
kk ln -f same sub/../same 2> /dev/null
printf 'ln-alias=%s content=%s\n' "$?" "$(cat same)"
echo other > replaced
kk ln -f same replaced
printf 'ln-replace=%s content=%s\n' "$?" "$(cat replaced)"

mkdir tree
echo leaf > tree/leaf
kk ln -s tree tree_link
kk cp -r tree tree_link 2> /dev/null
printf 'cp-through-link=%s directories=%s\n' "$?" "$(kk find tree -type d | kk wc -l | kk tr -d ' ')"

printf 'Hello, World\n' | kk tr a-z '[A*]'
printf 'abcdef\n' | kk tr a-f 'x[y*2]z[w*]'
printf 'abc\n' | kk tr a-c '[x*99999999999999]'

kk seq 1 7 | kk paste -d, - - -

printf 'x\ny' | kk sed '$a\appended'
printf 'x\ny' | kk sed 'a\each'

printf 'b x\na y\n' > fields
kk sort -k 9223372036854775807 fields
kk uniq -f 99999999999 fields

printf 'a1\nb2' | kk fold | kk od -c
printf 'abc' | kk od -j 4 2> /dev/null
printf 'od-past-end=%s\n' "$?"
