unset KOSH_FLAGS
# A non-interactive -WWW run keeps both reports, the analysis warning that
# lints even unreached branches and the runtime error, while the interactive
# prompt dedupes to the runtime error alone. Only the script path is
# assertable here, the count guards that the prompt silencing did not leak.
"$BIN" -WWW -c 'dasdsadsa_zzqq' 2>&1 | grep -c "dasdsadsa_zzqq.*found"
"$BIN" -c 'dasdsadsa_zzqq' 2>&1 | grep -c "dasdsadsa_zzqq.*found"
echo "rc=$?"
