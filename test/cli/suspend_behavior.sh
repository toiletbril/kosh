unset KOSH_FLAGS
# suspend stops the shell with SIGSTOP and resumes on SIGCONT. The login-shell
# guard is bypassed with -f. The test runs the shell in the background, sends
# SIGCONT after a pause, and checks that the line after suspend ran.
"$BIN" -c 'suspend -f; echo resumed' &
SUSP_PID=$!
sleep 1
kill -CONT "$SUSP_PID" 2>/dev/null
wait "$SUSP_PID"
echo "rc=$?"
