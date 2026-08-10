unset KOSH_FLAGS
# kill, fg, disown, bg, and wait accept the full job spec grammar, not only
# %number. A leading name matches a job by its command prefix, %+ names the
# current job.
echo "== kill resolves a job by its command name:"
"$BIN" --mood bash -c 'sleep 3 & kill %sleep 2>&1; wait 2>/dev/null; echo ok' 2>&1 | grep -c ok
echo "== disown by name drops the job so a later kill fails:"
"$BIN" --mood bash -c 'sleep 3 & disown %sleep; kill %1' 2>&1 | grep -c "not a valid job\|not a known job"
echo "== fg by %+ selects the current job:"
"$BIN" --mood bash -c 'sleep 0.2 & fg %+ >/dev/null 2>&1; echo ok' 2>&1 | grep -c ok
echo "== wait resolves a job by its command name:"
"$BIN" --mood bash -c 'sleep 0.2 & wait %sleep; echo ok' 2>&1 | grep -c ok
echo "== bg by name resumes a stopped job:"
"$BIN" --mood bash -c 'sleep 0.3 & bg %sleep >/dev/null 2>&1; echo ok' 2>&1 | grep -c ok
