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

printf 'echo $disk_only\n[ x == y ]\n' > "$directory/open-source.sh"
printf 'echo $disk_aux\n[ x == y ]\n' > "$directory/disk-source.sh"
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
  frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file:///tmp","capabilities":{"general":{"positionEncodings":["utf-8"]},"textDocument":{"publishDiagnostics":{"dataSupport":true},"codeAction":{"isPreferredSupport":true,"codeActionLiteralSupport":{"codeActionKind":{"valueSet":["quickfix","source.fixAll.kosh"]}}}}}}}'
  frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/server-test.shit","languageId":"shellscript","version":1,"text":"if\n"}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/server-test.shit","version":2},"contentChanges":[{"text":"#!/bin/sh\nvalue=one\nshow() { echo \"$value\"; }\nshow\nprintf ok\nls\nact\npath-only\nlater\nlater() { :; }\nformatted-command\necho \"$future\"\nfuture=one\nfuture=two\necho \"$future\"\nrepeat() { :; }\nrepeat() { :; }\nrepeat\nnever-defined\n[ x == y ]\n"}]}}'
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
  frame '{"jsonrpc":"2.0","id":21,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"range":{"start":{"line":19,"character":0},"end":{"line":19,"character":10}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","id":22,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"range":{"start":{"line":19,"character":0},"end":{"line":19,"character":10}},"context":{"diagnostics":[],"only":["source.fixAll.kosh"]}}}'
  frame '{"jsonrpc":"2.0","id":23,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"range":{"start":{"line":19,"character":0},"end":{"line":19,"character":10}},"context":{"diagnostics":[],"only":["refactor"]}}}'
  frame '{"jsonrpc":"2.0","id":9,"method":"unknown/request","params":{}}'
  frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file://$directory/open-source.sh\",\"languageId\":\"sh\",\"version\":1,\"text\":\"#!/bin/sh\\n[ x == y ]\\n\"}}}"
  frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file://$directory/open-root.sh\",\"languageId\":\"sh\",\"version\":1,\"text\":\"#!/bin/sh\\n. $directory/open-source.sh\\n\"}}}"
  frame "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"file://$directory/disk-root.sh\",\"languageId\":\"sh\",\"version\":1,\"text\":\"#!/bin/sh\\n. $directory/disk-source.sh\\n\"}}}"
  frame "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"textDocument/codeAction\",\"params\":{\"textDocument\":{\"uri\":\"file://$directory/open-root.sh\"},\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":1,\"character\":80}},\"context\":{\"diagnostics\":[],\"only\":[\"quickfix\"]}}}"
  frame "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"textDocument/codeAction\",\"params\":{\"textDocument\":{\"uri\":\"file://$directory/open-source.sh\"},\"range\":{\"start\":{\"line\":1,\"character\":0},\"end\":{\"line\":1,\"character\":10}},\"context\":{\"diagnostics\":[],\"only\":[\"quickfix\"]}}}"
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/utf8-action.sh","languageId":"sh","version":1,"text":"[ \"\ud83d\ude00\" == x ]\n"}}}'
  frame '{"jsonrpc":"2.0","id":26,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/utf8-action.sh"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":14}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/warning-action.bash","languageId":"bash","version":1,"text":"[[ 1 > 0 ]]\n"}}}'
  frame '{"jsonrpc":"2.0","id":27,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/warning-action.bash"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":11}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","id":28,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/warning-action.bash"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":11}},"context":{"diagnostics":[],"only":["source.fixAll.kosh"]}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/mood-shebang.sh","languageId":"sh","version":1,"text":"#!/bin/bash\n[[ 1 > 0 ]]\n"}}}'
  frame '{"jsonrpc":"2.0","id":32,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/mood-shebang.sh"},"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":11}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/mood-extension.sh","languageId":"bash","version":1,"text":"[[ 1 > 0 ]]\n"}}}'
  frame '{"jsonrpc":"2.0","id":33,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/mood-extension.sh"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":11}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/mood-shellscript.sh","languageId":"shellscript","version":1,"text":"[[ 1 > 0 ]]\n"}}}'
  frame '{"jsonrpc":"2.0","id":34,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/mood-shellscript.sh"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":11}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","id":29,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"range":{"start":{"line":19,"character":4},"end":{"line":19,"character":4}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","id":30,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"range":{"start":{"line":19,"character":6},"end":{"line":19,"character":6}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","id":31,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"},"range":{"start":{"line":19,"character":6},"end":{"line":19,"character":7}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh","languageId":"bash","version":1,"text":"#!/bin/bash\nname=first\nif [ -n \"$1\" ]; then\n  name=$1\nfi\nname=second\nprintf %s \"$name\"\ngreet() {\n  printf %s \"$name\"\n}\nfunction greet {\n  printf other\n}\ngreet\nlist=(a b)\nlist+=(c)\ntotal=one\ntotal+=two\necho $unseen\nlater\nlater() { :; }\n"}}}'
  frame '{"jsonrpc":"2.0","id":60,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":6,"character":11}}}'
  frame '{"jsonrpc":"2.0","id":61,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":5,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":62,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":8,"character":14}}}'
  frame '{"jsonrpc":"2.0","id":63,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":13,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":64,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":7,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":65,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":10,"character":10}}}'
  frame '{"jsonrpc":"2.0","id":66,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":15,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":67,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":17,"character":1}}}'
  frame '{"jsonrpc":"2.0","id":68,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":18,"character":6}}}'
  frame '{"jsonrpc":"2.0","id":69,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-test.sh"},"position":{"line":19,"character":1}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didClose","params":{"textDocument":{"uri":"file:///tmp/server-test.shit"}}}'
  frame '{"jsonrpc":"2.0","id":4,"method":"shutdown","params":null}'
  frame '{"jsonrpc":"2.0","method":"exit"}'
} > "$directory/input"

output=$(PATH="$directory/bin:$PATH" "$BIN" --language-server < "$directory/input")
status=$?
printf 'status=%s\n' "$status"
check_contains initialize '"positionEncoding":"utf-8"'
check_contains code-action-capability '"codeActionKinds":["quickfix","source.fixAll.kosh"]'
check_contains diagnostics '"severity":1'
check_contains completion '"label":"printf"'
check_contains semantic-legend '"heredocDelimiter"'
check_contains semantic-tokens '"id":3,"result":{"data":['
check_contains variable-definition '"id":10,"result":{"uri":"file:///tmp/server-test.shit","range":{"start":{"line":1,"character":0},"end":{"line":1,"character":5}}}'
check_contains function-definition '"id":11,"result":{"uri":"file:///tmp/server-test.shit","range":{"start":{"line":2,"character":0},"end":{"line":2,"character":4}}}'
check_contains forward-function-limit '"id":16,"result":null'
check_contains forward-variable-limit '"id":18,"result":null'
check_contains latest-variable-definition '"id":19,"result":{"uri":"file:///tmp/server-test.shit","range":{"start":{"line":13,"character":0},"end":{"line":13,"character":6}}}'
check_contains latest-function-definition '"id":20,"result":{"uri":"file:///tmp/server-test.shit","range":{"start":{"line":16,"character":0},"end":{"line":16,"character":6}}}'
check_contains diagnostic-fix-data '"data":{"kind":"kosh.fix","documentVersion":2'
check_contains quick-fix "\"id\":21,\"result\":[{\"title\":\"Replace '==' with '='\",\"kind\":\"quickfix\",\"isPreferred\":true,\"edit\":{\"changes\":{\"file:///tmp/server-test.shit\":[{\"range\":{\"start\":{\"line\":19,\"character\":4},\"end\":{\"line\":19,\"character\":6}},\"newText\":\"=\"}]}}}"
check_contains fix-all '"id":22,"result":[{"title":"Fix all safe kosh diagnostics","kind":"source.fixAll.kosh","isPreferred":true,"edit":{"changes":{"file:///tmp/server-test.shit":[{"range":{"start":{"line":19,"character":4},"end":{"line":19,"character":6}},"newText":"="}]}}}'
check_contains filtered-actions '"id":23,"result":[]'
check_contains root-child-action '"id":24,"result":[]'
check_contains open-child-action "\"id\":25,\"result\":[{\"title\":\"Replace '==' with '='\",\"kind\":\"quickfix\",\"isPreferred\":true,\"edit\":{\"changes\":{\"file://$directory/open-source.sh\":[{\"range\":{\"start\":{\"line\":1,\"character\":4},\"end\":{\"line\":1,\"character\":6}},\"newText\":\"=\"}]}}}"
check_contains utf8-code-action '"id":26,"result":[{"title":"Replace '\''=='\'' with '\''='\''","kind":"quickfix","isPreferred":true,"edit":{"changes":{"file:///tmp/utf8-action.sh":[{"range":{"start":{"line":0,"character":9},"end":{"line":0,"character":11}},"newText":"="}]}}}'
check_contains warning-code-action '"id":27,"result":[{"title":"Replace '\''>'\'' with '\''-gt'\''","kind":"quickfix","isPreferred":true,"edit":{"changes":{"file:///tmp/warning-action.bash":[{"range":{"start":{"line":0,"character":5},"end":{"line":0,"character":6}},"newText":"-gt"}]}}}'
check_contains unsafe-fix-all '"id":28,"result":[]'
check_contains shebang-over-language-id '"id":32,"result":[{"title":"Replace '\''>'\'' with '\''-gt'\''","kind":"quickfix","isPreferred":true,"edit":{"changes":{"file:///tmp/mood-shebang.sh":[{"range":{"start":{"line":1,"character":5},"end":{"line":1,"character":6}},"newText":"-gt"}]}}}'
check_contains language-id-over-extension '"id":33,"result":[{"title":"Replace '\''>'\'' with '\''-gt'\''","kind":"quickfix","isPreferred":true,"edit":{"changes":{"file:///tmp/mood-extension.sh":[{"range":{"start":{"line":0,"character":5},"end":{"line":0,"character":6}},"newText":"-gt"}]}}}'
check_contains shellscript-language-id '"id":34,"result":[{"title":"Replace '\''>'\'' with '\''-gt'\''","kind":"quickfix","isPreferred":true,"edit":{"changes":{"file:///tmp/mood-shellscript.sh":[{"range":{"start":{"line":0,"character":5},"end":{"line":0,"character":6}},"newText":"-gt"}]}}}'
check_contains action-start-cursor '"id":29,"result":[{"title":"Replace '\''=='\'' with '\''='\''"'
check_contains action-end-cursor '"id":30,"result":[]'
check_contains action-adjacent-range '"id":31,"result":[]'
check_contains builtin-help '"id":12,"result":{"contents":{"kind":"plaintext","value":"DESCRIPTION\n  The printf builtin'
check_contains utility-help '"id":13,"result":{"contents":{"kind":"plaintext","value":"DESCRIPTION\n  The ls utility'
check_contains allowed-help 'allowed command help'
check_contains path-fallback "Path: $directory/bin/path-only"
check_contains manpage-text '"id":17,"result":{"contents":{"kind":"plaintext","value":"BOLD\n\nPath:'
case $output in
*'\b'*) printf 'manpage-overstrike=present\n' ;;
*) printf 'manpage-overstrike=ok\n' ;;
esac
check_contains hover-variable-value '"id":60,"result":{"contents":{"kind":"plaintext","value":"name=second\nValue: second\n\nEarlier assignments:\nline 4: name=$1 (conditional)\nline 2: name=first"}'
check_contains hover-assignment-name '"id":61,"result":{"contents":{"kind":"plaintext","value":"name=second\nValue: second\n\nEarlier assignments:'
check_contains hover-variable-in-body '"id":62,"result":{"contents":{"kind":"plaintext","value":"name=second\nValue: second\n\nEarlier assignments:'
check_contains hover-latest-body '"id":63,"result":{"contents":{"kind":"plaintext","value":"greet () \n{\n  printf other\n}"}'
check_contains hover-first-body '"id":64,"result":{"contents":{"kind":"plaintext","value":"greet () \n{\n  printf %s \"$name\"\n}"}'
check_contains hover-keyword-body '"id":65,"result":{"contents":{"kind":"plaintext","value":"greet () \n{\n  printf other\n}"}'
check_contains hover-array-value '"id":66,"result":{"contents":{"kind":"plaintext","value":"list+=(c)\nThe value is a list, and the elements are not folded.\n\nEarlier assignments:\nline 15: list=(a b)"}'
check_contains hover-append-value '"id":67,"result":{"contents":{"kind":"plaintext","value":"total+=two\nThe value appends to what came before.\n\nEarlier assignments:\nline 17: total=one"}'
check_contains hover-unassigned-variable '"id":68,"result":null'
check_contains hover-forward-function '"id":69,"result":null'
check_contains method-error '"id":9,"error":{"code":-32601'
check_contains auxiliary-uri "\"uri\":\"file://$directory/disk-source.sh\""
check_contains auxiliary-diagnostic disk_aux
check_contains open-source-version "\"uri\":\"file://$directory/open-source.sh\",\"version\":1"
check_contains primary-message '"message":"An unquoted variable can split into words and expand globs. (SC2086)"'
check_contains detail-information '"severity":3,"source":"kosh","message":"Quote the expansion to keep one argument"'
check_contains dynamic-command-information '"severity":3,"source":"kosh","message":"This command may be defined dynamically or outside this script"'
case $output in
*disk_only*) printf 'open-source-precedence=missing\n' ;;
*) printf 'open-source-precedence=ok\n' ;;
esac
disk_payload=${output#*"\"uri\":\"file://$directory/disk-source.sh\""}
disk_payload=${disk_payload%%Content-Length:*}
case $disk_payload in
*'"data":'*) printf 'disk-fix-data=present\n' ;;
*) printf 'disk-fix-data=ok\n' ;;
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
    frame '{"jsonrpc":"2.0","id":5,"method":"initialize","params":{"capabilities":{"workspace":{"workspaceEdit":{"documentChanges":true}},"textDocument":{"publishDiagnostics":{"dataSupport":true},"codeAction":{"isPreferredSupport":true,"codeActionLiteralSupport":{"codeActionKind":{"valueSet":["quickfix","source.fixAll.kosh"]}}}}}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/utf16.sh","languageId":"sh","version":1,"text":"#\ud83d\ude00\r\n"}}}'
    frame '{"jsonrpc":"2.0","id":6,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///tmp/utf16.sh"}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/versioned.sh","languageId":"sh","version":1,"text":"#!/bin/sh\n[ \"$1\" == y ]\n"}}}'
    frame '{"jsonrpc":"2.0","id":8,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/versioned.sh"},"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":13}},"context":{"diagnostics":[{"range":{"start":{"line":1,"character":7},"end":{"line":1,"character":9}},"code":"posix-test-equals","message":"current","data":{"kind":"kosh.fix","documentVersion":1,"diagnosticRevision":1,"diagnosticIndex":0}}],"only":["quickfix"]}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/revision-trigger.sh","languageId":"sh","version":1,"text":"#!/bin/sh\n"}}}'
    frame '{"jsonrpc":"2.0","id":11,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/versioned.sh"},"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":13}},"context":{"diagnostics":[{"range":{"start":{"line":1,"character":7},"end":{"line":1,"character":9}},"code":"posix-test-equals","message":"stale","data":{"kind":"kosh.fix","documentVersion":1,"diagnosticRevision":1,"diagnosticIndex":0}}],"only":["quickfix"]}}}'
    frame '{"jsonrpc":"2.0","id":12,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/versioned.sh"},"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":13}},"context":{"diagnostics":[{"range":{"start":{"line":1,"character":7},"end":{"line":1,"character":9}},"code":"posix-test-equals","message":"current","data":{"kind":"kosh.fix","documentVersion":1,"diagnosticRevision":1,"diagnosticIndex":0}}],"only":["quickfix"]}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didChange","params":{"textDocument":{"uri":"file:///tmp/versioned.sh","version":2},"contentChanges":[{"text":"#!/bin/sh\n[ \"$1\" = y ]\n"}]}}'
    frame '{"jsonrpc":"2.0","id":9,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/versioned.sh"},"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":12}},"context":{"diagnostics":[{"range":{"start":{"line":1,"character":7},"end":{"line":1,"character":9}},"code":"posix-test-equals","message":"stale","data":{"kind":"kosh.fix","documentVersion":1,"diagnosticRevision":1,"diagnosticIndex":0}}],"only":["quickfix"]}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/utf16-action.sh","languageId":"sh","version":1,"text":"[ \"\ud83d\ude00\" == x ]\r\n"}}}'
    frame '{"jsonrpc":"2.0","id":10,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/utf16-action.sh"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":12}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
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
case $utf16_output in
*'"id":8,"result":[{"title":"Replace'*'"documentChanges":[{"textDocument":{"uri":"file:///tmp/versioned.sh","version":1},"edits":[{"range":{"start":{"line":1,"character":7},"end":{"line":1,"character":9}},"newText":"="}]}'*)
  printf 'versioned-code-action=ok\n'
  ;;
*) printf 'versioned-code-action=missing\n' ;;
esac
case $utf16_output in
*'"id":8,"result":[{"title":"Replace'*'"diagnostics":[{"range":{"start":{"line":1,"character":7},"end":{"line":1,"character":9}},"code":"posix-test-equals","message":"current","data":{"kind":"kosh.fix","documentVersion":1,"diagnosticRevision":1,"diagnosticIndex":0}}]'*)
  printf 'current-diagnostic-identity=ok\n'
  ;;
*) printf 'current-diagnostic-identity=missing\n' ;;
esac
stale_identity_action=${utf16_output#*'"id":11,"result":'}
stale_identity_action=${stale_identity_action%%Content-Length:*}
case $stale_identity_action in
*'"title":"Replace'*'"diagnosticRevision":1'*)
  printf 'stable-diagnostic-identity=ok\n'
  ;;
*) printf 'stable-diagnostic-identity=missing\n' ;;
esac
case $utf16_output in
*'"id":12,"result":[{"title":"Replace'*'"diagnosticRevision":1'*)
  printf 'unrelated-open-keeps-revision=ok\n'
  ;;
*) printf 'unrelated-open-keeps-revision=missing\n' ;;
esac
case $utf16_output in
*'"id":9,"result":[]'*) printf 'stale-code-action=ok\n' ;;
*) printf 'stale-code-action=missing\n' ;;
esac
case $utf16_output in
*'"id":10,"result":[{"title":"Replace '\''=='\'' with '\''='\''"'*'"range":{"start":{"line":0,"character":7},"end":{"line":0,"character":9}},"newText":"="'*)
  printf 'utf16-code-action=ok\n'
  ;;
*) printf 'utf16-code-action=missing\n' ;;
esac

markdown_output=$(
  {
    frame '{"jsonrpc":"2.0","id":70,"method":"initialize","params":{"capabilities":{"general":{"positionEncodings":["utf-8"]},"textDocument":{"hover":{"contentFormat":["markdown","plaintext"]}}}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/hover-markdown.sh","languageId":"bash","version":1,"text":"#!/bin/bash\nname=one\nname=two\necho \"$name\"\nshow() {\n  printf ok\n}\nshow\n"}}}'
    frame '{"jsonrpc":"2.0","id":71,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-markdown.sh"},"position":{"line":3,"character":6}}}'
    frame '{"jsonrpc":"2.0","id":72,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/hover-markdown.sh"},"position":{"line":7,"character":1}}}'
    frame '{"jsonrpc":"2.0","id":73,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit"}'
  } | "$BIN" --language-server
)
case $markdown_output in
*'"id":71,"result":{"contents":{"kind":"markdown","value":"```shell\nname=two\n```\nValue: two\n\nEarlier assignments:\n\n- line 2: `name=one`"}'*)
  printf 'markdown-variable-hover=ok\n'
  ;;
*) printf 'markdown-variable-hover=missing\n' ;;
esac
case $markdown_output in
*'"id":72,"result":{"contents":{"kind":"markdown","value":"```shell\nshow () \n{\n  printf ok\n}\n```"}'*)
  printf 'markdown-function-hover=ok\n'
  ;;
*) printf 'markdown-function-hover=missing\n' ;;
esac

unsupported_output=$(
  {
    frame '{"jsonrpc":"2.0","id":30,"method":"initialize","params":{"capabilities":{}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/unsupported.sh","languageId":"sh","version":1,"text":"[ x == y ]\n"}}}'
    frame '{"jsonrpc":"2.0","id":31,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/unsupported.sh"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":10}},"context":{"diagnostics":[]}}}'
    frame '{"jsonrpc":"2.0","id":32,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit"}'
  } | "$BIN" --language-server
)
case $unsupported_output in
*'codeActionProvider'*) printf 'unsupported-capability=present\n' ;;
*) printf 'unsupported-capability=ok\n' ;;
esac
case $unsupported_output in
*'"id":31,"result":[]'*) printf 'unsupported-code-action=ok\n' ;;
*) printf 'unsupported-code-action=missing\n' ;;
esac

limited_output=$(
  {
    frame '{"jsonrpc":"2.0","id":40,"method":"initialize","params":{"capabilities":{"textDocument":{"codeAction":{"codeActionLiteralSupport":{"codeActionKind":{"valueSet":["quickfix"]}}}}}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/limited.sh","languageId":"sh","version":1,"text":"[ x == y ]\n"}}}'
    frame '{"jsonrpc":"2.0","id":41,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/limited.sh"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":10}},"context":{"diagnostics":[],"only":["quickfix"]}}}'
    frame '{"jsonrpc":"2.0","id":42,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/limited.sh"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":10}},"context":{"diagnostics":[],"only":["source.fixAll.kosh"]}}}'
    frame '{"jsonrpc":"2.0","id":43,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit"}'
  } | "$BIN" --language-server
)
case $limited_output in
*'"codeActionKinds":["quickfix"]'*'"id":41,"result":[{"title":"Replace'*'"id":42,"result":[]'*)
  printf 'limited-actions=ok\n'
  ;;
*) printf 'limited-actions=missing\n' ;;
esac
case $limited_output in
*'"data":'*|*'"isPreferred":'*) printf 'optional-action-fields=present\n' ;;
*) printf 'optional-action-fields=ok\n' ;;
esac

malformed_output=$(
  {
    frame '{"jsonrpc":"2.0","id":50,"method":"initialize","params":{"capabilities":{"textDocument":{"codeAction":{"codeActionLiteralSupport":{}}}}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/malformed.sh","languageId":"sh","version":1,"text":"[ x == y ]\n"}}}'
    frame '{"jsonrpc":"2.0","id":51,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/malformed.sh"},"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":10}},"context":{"diagnostics":[]}}}'
    frame '{"jsonrpc":"2.0","id":52,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit"}'
  } | "$BIN" --language-server
)
case $malformed_output in
*'codeActionProvider'*) printf 'malformed-actions=present\n' ;;
*'"id":51,"result":[]'*) printf 'malformed-actions=ok\n' ;;
*) printf 'malformed-actions=missing\n' ;;
esac

frame '{"jsonrpc":"2.0","method":"exit"}' |
  "$BIN" --language-server > /dev/null
printf 'exit-without-shutdown-status=%s\n' "$?"
