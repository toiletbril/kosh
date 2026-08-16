unset KOSH_FLAGS
# export of a read-only name fails, and export of an integer-marked name stores
# the evaluated arithmetic value in the environment a child reads.
echo "== export of a read-only name fails:"
"$BIN" -c 'readonly x=old; export x=new'; echo "rc=$?"
echo "== a bare export publishes a read-only value:"
"$BIN" -c 'readonly x=old; export x; koshkit env | grep "^x=old$"'
echo "== export of an integer-marked name stores the evaluated value:"
"$BIN" -c 'declare -i n=3+4; export n; echo "$n"'
