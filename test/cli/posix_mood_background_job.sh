unset KOSH_FLAGS
# The sh mood and the equivalent --posix flag support background jobs and the
# jobs builtin, the same job table the default and the bash moods use.
echo "== sh mood lists a running background job:"
"$BIN" --mood sh -c 'sleep 1 & jobs; wait; echo DONE' 2>&1 | grep -c Running
echo "== sh mood waits for it and completes:"
"$BIN" --mood sh -c 'sleep 1 & jobs; wait; echo DONE' 2>&1 | grep -c DONE
echo "== --posix behaves the same as --mood sh:"
"$BIN" --posix -c 'sleep 1 & jobs; wait; echo DONE' 2>&1 | grep -c DONE
