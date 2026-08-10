unset KOSH_FLAGS
# POSIX mode names the owning dialect when a bashism trips its parse.
"$BIN" --mood sh -c 'true |& cat' 2>&1 | head -1
"$BIN" --mood sh -c 'for ((i=0;i<2;i++)); do echo $i; done' 2>&1 | head -1
"$BIN" --mood sh -c 'read -r x <<< hi' 2>&1 | head -1
echo "rc=$?"
