#!/bin/bash
# With SIGPIPE ignored by trap '', a pipeline stage keeps it ignored, so a
# builtin or program writing to a closed pipe fails with a write error and
# goes on instead of dying from the signal. Clearing the trap restores the
# default, and the stage dies with status 141 again, all as in bash.
trap '' PIPE
{ for ((i = 1; i <= 100000; i++)); do echo "$i" || { echo "stopped" >&2; break; }; done; } | head -1
echo "builtin-ignored=${PIPESTATUS[*]}"
( sleep 0.2; echo late; echo "subshell=$?" >&2 ) | true
yes | head -1
echo "program-ignored=${PIPESTATUS[*]}"
trap - PIPE
{ for ((i = 1; i <= 100000; i++)); do echo "$i"; done; } | head -1
echo "builtin-default=${PIPESTATUS[*]}"
yes | head -1
echo "program-default=${PIPESTATUS[*]}"
