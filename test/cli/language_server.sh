directory=$(mktemp -d) || exit 1
trap '[ -n "$directory" ] && /bin/rm -rf "$directory"' EXIT

frame()
{
  local body=$1
  printf 'Content-Length: %s\r\n\r\n%s' "${#body}" "$body"
}

check_contains()
{
  local label=$1
  local needle=$2
  case $output in
  *"$needle"*) printf '%s=ok\n' "$label" ;;
  *) printf '%s=missing\n' "$label" ;;
  esac
}

printf 'echo $disk_only\n' > "$directory/open-source.sh"
printf 'echo $disk_aux\n' > "$directory/disk-source.sh"
mkdir "$directory/bin"
printf '#!/bin/sh\nprintf "allowed command help\\n"\n' > "$directory/bin/act"
printf '#!/bin/sh\nexit 0\n' > "$directory/bin/path-only"
printf '#!/bin/sh\nexit 0\n' > "$directory/bin/formatted-command"
printf '%s\n' '#!/bin/sh' \
  'if [ "$1" = -w ]; then' \
  '  case $2 in' \
  "  formatted-command) printf '/private/formatted-command.1\\n' ;;" \
  '  esac' \
  '  exit' \
  'fi' \
  "case \$1 in formatted-command) printf 'B\\bBO\\bOL\\bLD\\bD\\n' ;; esac" \
  > "$directory/bin/man"
chmod +x "$directory/bin/act" "$directory/bin/path-only" \
  "$directory/bin/formatted-command" "$directory/bin/man"

{
  frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file:///tmp","capabilities":{"general":{"positionEncodings":["utf-8"]}}}}'
  frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/server-test.shit","languageId":"shellscript","version":1,"text":"if\n"}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/server-test.shit","version":2},"contentChanges":[{"text":"#!/bin/sh\nvalue=one\nshow() { echo \"$value\"; }\nshow\nprintf ok\nls\nact\npath-only\nlater\nlater() { :; }\nformatted-command\necho \"$future\"\nfuture=one\nfuture=two\necho \"$future\"\nrepeat() { :; }\nrepeat() { :; }\nrepeat\nnever-defined\n"}]}}'
  frame '{"jsonrpc":"2.0","id":2,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":4,"character":2}}}'
  frame '{"jsonrpc":"2.0","id":3,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"}}}'
  frame '{"jsonrpc":"2.0","id":10,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":2,"character":17}}}'
  frame '{"jsonrpc":"2.0","id":11,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":3,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":12,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":4,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":13,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":5,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":14,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":6,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":15,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":7,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":16,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":8,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":17,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":10,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":18,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":11,"character":8}}}'
  frame '{"jsonrpc":"2.0","id":19,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":14,"character":8}}}'
  frame '{"jsonrpc":"2.0","id":20,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"position":{"line":17,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":9,"method":"unknown/request","params":{}}'
  frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file://$directory/open-source.sh\",\"languageId\":\"sh\",\"version\":1,\"text\":\"#!/bin/sh\\necho open\\n\"}}}"
  frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file://$directory/open-root.sh\",\"languageId\":\"sh\",\"version\":1,\"text\":\"#!/bin/sh\\n. $directory/open-source.sh\\n\"}}}"
  frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file://$directory/disk-root.sh\",\"languageId\":\"sh\",\"version\":1,\"text\":\"#!/bin/sh\\n. $directory/disk-source.sh\\n\"}}}"
  frame '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"}}}'
  frame '{"jsonrpc":"2.0","id":4,"method":"shutdown","params":null}'
  frame '{"jsonrpc":"2.0","method":"exit"}'
} > "$directory/input"

output=$(PATH="$directory/bin:$PATH" "$BIN" --language-server < "$directory/input")
status=$?
printf 'status=%s\n' "$status"
check_contains initialize '"positionEncoding":"utf-8"'
check_contains diagnostics '"severity":1'
check_contains completion '"label":"printf"'
check_contains semantic-tokens '"id":3,"result":{"data":['
check_contains variable-definition '"id":10,"result":{"uri":"file:///tmp/server-test.shit","range":{"start":{"line":1,"character":0},"end":{"line":1,"character":5}}}'
check_contains function-definition '"id":11,"result":{"uri":"file:///tmp/server-test.shit","range":{"start":{"line":2,"character":0},"end":{"line":2,"character":4}}}'
check_contains forward-function-limit '"id":16,"result":null'
check_contains forward-variable-limit '"id":18,"result":null'
check_contains latest-variable-definition '"id":19,"result":{"uri":"file:///tmp/server-test.shit","range":{"start":{"line":13,"character":0},"end":{"line":13,"character":6}}}'
check_contains latest-function-definition '"id":20,"result":{"uri":"file:///tmp/server-test.shit","range":{"start":{"line":16,"character":0},"end":{"line":16,"character":6}}}'
check_contains builtin-help '"id":12,"result":{"contents":{"kind":"plaintext","value":"DESCRIPTION\n  The printf builtin'
check_contains utility-help '"id":13,"result":{"contents":{"kind":"plaintext","value":"DESCRIPTION\n  The ls utility'
check_contains allowed-help 'allowed command help'
check_contains path-fallback "Path: $directory/bin/path-only"
check_contains manpage-text '"id":17,"result":{"contents":{"kind":"plaintext","value":"BOLD\n\nPath:'
case $output in
*'\b'*) printf 'manpage-overstrike=present\n' ;;
*) printf 'manpage-overstrike=ok\n' ;;
esac
check_contains method-error '"id":9,"error":{"code":-32601'
check_contains auxiliary-uri "\"uri\":\"file://$directory/disk-source.sh\""
check_contains auxiliary-diagnostic disk_aux
check_contains primary-message '"message":"An unquoted variable can split into words and expand globs. (SC2086)"'
check_contains detail-information '"severity":3,"source":"kosh","message":"Quote the expansion to keep one argument"'
check_contains dynamic-command-information '"severity":3,"source":"kosh","message":"This command may be defined dynamically or outside this script"'
case $output in
*disk_only*) printf 'open-source-precedence=missing\n' ;;
*) printf 'open-source-precedence=ok\n' ;;
esac

after_first_clear=${output#*'"diagnostics":[]'}
after_second_clear=${after_first_clear#*'"diagnostics":[]'}
if [ "$after_first_clear" != "$output" ] &&
   [ "$after_second_clear" != "$after_first_clear" ]; then
  printf 'diagnostic-clears=ok\n'
else
  printf 'diagnostic-clears=missing\n'
fi

KOSH_FLAGS=--language-server "$BIN" -c 'echo environment-filtered'

"$BIN" --language-server script.sh > "$directory/conflict-out" 2>&1
conflict_status=$?
printf 'conflict-status=%s\n' "$conflict_status"
case $(< "$directory/conflict-out") in
*"does not accept"*) printf 'conflict-message=ok\n' ;;
*) printf 'conflict-message=missing\n' ;;
esac

utf16_output=$(
  {
    frame '{"jsonrpc":"2.0","id":5,"method":"initialize","params":{"capabilities":{}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/utf16.sh","languageId":"sh","version":1,"text":"#\ud83d\ude00\r\n"}}}'
    frame '{"jsonrpc":"2.0","id":6,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///tmp/utf16.sh"}}}'
    frame '{"jsonrpc":"2.0","id":7,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit"}'
  } | "$BIN" --language-server
)
case $utf16_output in
*'"positionEncoding":"utf-16"'*'"id":6,"result":{"data":[0,0,3,0,0]}'*)
  printf 'utf16-crlf=ok\n'
  ;;
*) printf 'utf16-crlf=missing\n' ;;
esac

frame '{"jsonrpc":"2.0","method":"exit"}' |
  "$BIN" --language-server > /dev/null
printf 'exit-without-shutdown-status=%s\n' "$?"
