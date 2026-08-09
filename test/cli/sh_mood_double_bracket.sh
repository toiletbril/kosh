unset SHIT_FLAGS
# The [[ conditional is a bash addition, so the sh mood rejects it while the
# default and bash moods run it.
"$BIN" -c 'value=x; [[ -n $value ]] && echo ran'
"$BIN" --mood bash -c 'value=x; [[ -n $value ]] && echo ran'
"$BIN" --mood sh -c 'value=x; [[ -n $value ]] && echo ran' 2>&1 | head -1
echo "rc=$?"
