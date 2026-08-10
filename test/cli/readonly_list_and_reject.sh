unset KOSH_FLAGS
# readonly lists the marked variables, marks a name with or without a value, and
# a later assignment to a marked name fails. The list output is filtered to the
# test's own names so an unrelated default read-only variable cannot perturb it.
echo "== list shows a name marked with a value:"
"$BIN" -c 'readonly x=5; readonly' 2>&1 | grep "readonly x="
echo "== reassignment of a read-only name is rejected:"
"$BIN" -c 'readonly x=5; x=6'; echo "rc=$?"
echo "== a bare name marks the current value:"
"$BIN" -c 'y=hi; readonly y; y=bye'; echo "rc=$?"
echo "== a bare-marked name appears in the list:"
"$BIN" -c 'z=1; readonly z; readonly' 2>&1 | grep "readonly z="
