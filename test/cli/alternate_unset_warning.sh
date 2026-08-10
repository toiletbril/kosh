unset KOSH_FLAGS
# The ${name:+word} alternate form tests whether a name is set, so an unset name
# expands to empty by design and never warns, not even under -W. Each grep count
# is zero because no warning is printed.
echo "== -W is silent on an unset :+:"
"$BIN" -W -c 'echo "[${UNSETV:+x}]"' 2>&1 | grep -c "is not set"
echo "== plain run is silent:"
"$BIN" --mood bash -c 'echo "[${UNSETV:+x}]"' 2>&1 | grep -c "is not set"
echo "== a set name is silent under -W:"
"$BIN" -W -c 'V=val; echo "[${V:+y}]"' 2>&1 | grep -c "is not set"
