unset KOSH_FLAGS
echo "== a soft builtin error in the bash mood carries a located caret =="
"$BIN" --mood bash -c 'builtin nosuchbuiltin'
echo "rc=$?"
echo "== the run keeps going past the soft error =="
"$BIN" --mood bash -c 'builtin nosuchbuiltin; echo after'
echo "== a thrown builtin error in the bash mood is located and soft too =="
"$BIN" --mood bash -c 'read -Z; echo after'
