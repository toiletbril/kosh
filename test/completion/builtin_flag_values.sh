# A flag whose value comes from a closed set answers from that table instead of
# the filename fallback. The spaced form and the joined equals form are both
# accepted. HUP, INT, QUIT, KILL, and TERM are the signal names every platform
# carries, so the remaining names stay behind the POSIX guard.
echo "== kill -s signal names:"
"$BIN" --debug-complete-at 'kill -s ' </dev/null |
  grep -E '^(HUP|INT|KILL|QUIT|TERM)$'
echo "== kill -s prefix:"
"$BIN" --debug-complete-at 'kill -s IN' </dev/null
echo "== kill -n prefix:"
"$BIN" --debug-complete-at 'kill -n TE' </dev/null
echo "== trap special conditions:"
"$BIN" --debug-complete-at 'trap handler E' </dev/null
echo "== trap condition after the print flag:"
"$BIN" --debug-complete-at 'trap -p RET' </dev/null
echo "== trap action position is a filename:"
"$BIN" --debug-complete-at 'trap ' </dev/null | grep -c '^EXIT$'
echo "== shopt -o reads the set option names:"
"$BIN" --debug-complete-at 'shopt -o pipe' </dev/null
echo "== shopt operand stays a shopt name:"
"$BIN" --debug-complete-at 'shopt xpg' </dev/null
echo "== enable builtin names:"
"$BIN" --debug-complete-at 'enable ech' </dev/null
echo "== complete -o options:"
"$BIN" --debug-complete-at 'complete -o ' </dev/null
echo "== compgen -o joined form:"
"$BIN" --debug-complete-at 'compgen -o=d' </dev/null
echo "== koshkit find entry types:"
"$BIN" --debug-complete-at 'koshkit find -type ' </dev/null
echo "== koshkit timeout signal prefix:"
"$BIN" --debug-complete-at 'koshkit timeout -s QU' </dev/null
echo "== koshkit pkill joined signal form:"
"$BIN" --debug-complete-at 'koshkit pkill --signal=TE' </dev/null
echo "== koshkit killall signal prefix:"
"$BIN" --debug-complete-at 'koshkit killall -s KI' </dev/null
echo "== debug logging levels:"
"$BIN" --debug-complete-at 'kosh -X ' </dev/null
echo "== debug logging joined form:"
"$BIN" --debug-complete-at 'kosh --debug-logging=de' </dev/null
if [ "${OS-}" != Windows_NT ]; then
  echo "== posix signal names:"
  "$BIN" --debug-complete-at 'kill -s AL' </dev/null
  "$BIN" --debug-complete-at 'trap handler USR' </dev/null
  "$BIN" --debug-complete-at 'koshkit timeout --signal=AB' </dev/null
fi
