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
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/slash-function.sh","languageId":"bash","version":1,"text":"#!/bin/bash\nfunction ble/util/put {\n  printf %s \"$1\"\n}\nble/util/put hello\nble/string#quote() { :; }\nble/string#quote x\n/bin/sh -c true\n"}}}'
  frame '{"jsonrpc":"2.0","id":70,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/slash-function.sh"},"position":{"line":4,"character":4}}}'
  frame '{"jsonrpc":"2.0","id":71,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/slash-function.sh"},"position":{"line":4,"character":4}}}'
  frame '{"jsonrpc":"2.0","id":72,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/slash-function.sh"},"position":{"line":6,"character":4}}}'
  frame '{"jsonrpc":"2.0","id":73,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/slash-function.sh"},"position":{"line":7,"character":2}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/outline.sh","languageId":"bash","version":1,"text":"#!/bin/bash\ntop=1\nfunction outer {\n  local inner=2\n  inner=3\n}\nhelper() {\n  count=0\n}\ntop=4\n"}}}'
  frame '{"jsonrpc":"2.0","id":74,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/outline.sh"}}}'
  frame '{"jsonrpc":"2.0","id":75,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/absent.sh"}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/redirected-body.sh","languageId":"bash","version":1,"text":"#!/bin/bash\nquiet() { echo hi; } >/dev/null 2>&1\nquiet\nclosed() { :; } 2>&-\nclosed\n"}}}'
  frame '{"jsonrpc":"2.0","id":76,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/redirected-body.sh"},"position":{"line":2,"character":2}}}'
  frame '{"jsonrpc":"2.0","id":77,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/redirected-body.sh"},"position":{"line":4,"character":2}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/declare-family.sh","languageId":"bash","version":1,"text":"#!/bin/bash\ndeclare plain=1\nexport shipped=2\nreadonly frozen+=tail\ndeclare -a listed=(a b)\ntypeset slot[3]=q\nuse() {\n  local scoped=inner\n  echo \"$scoped\"\n}\necho \"$plain$shipped$frozen$listed$slot\"\n"}}}'
  frame '{"jsonrpc":"2.0","id":78,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/declare-family.sh"}}}'
  frame '{"jsonrpc":"2.0","id":79,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/declare-family.sh"},"position":{"line":8,"character":10}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/loop-binding.sh","languageId":"bash","version":1,"text":"#!/bin/bash\nfor entry in a b c; do\n  echo \"$entry\"\ndone\nselect choice in one two; do\n  echo \"$choice\"\ndone\n"}}}'
  frame '{"jsonrpc":"2.0","id":80,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/loop-binding.sh"}}}'
  frame '{"jsonrpc":"2.0","id":81,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/loop-binding.sh"},"position":{"line":2,"character":10}}}'
  frame '{"jsonrpc":"2.0","id":82,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/loop-binding.sh"},"position":{"line":5,"character":10}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/builtin-target.sh","languageId":"bash","version":1,"text":"#!/bin/bash\nread -n 5 field\nmapfile -t rows < listing\ngetopts \"ab:\" letter\nprintf -v joined \"%s\" x\nprintf -vpacked \"%s\" y\necho \"$field$rows$letter$joined$packed\"\n"}}}'
  frame '{"jsonrpc":"2.0","id":83,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/builtin-target.sh"}}}'
  frame '{"jsonrpc":"2.0","id":84,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/builtin-target.sh"},"position":{"line":6,"character":9}}}'
  frame '{"jsonrpc":"2.0","id":85,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/builtin-target.sh"},"position":{"line":6,"character":15}}}'
  frame '{"jsonrpc":"2.0","id":86,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/builtin-target.sh"},"position":{"line":6,"character":22}}}'
  frame '{"jsonrpc":"2.0","id":87,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/builtin-target.sh"},"position":{"line":6,"character":28}}}'
  frame '{"jsonrpc":"2.0","id":88,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/builtin-target.sh"},"position":{"line":6,"character":34}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/arithmetic-target.sh","languageId":"bash","version":1,"text":"#!/bin/bash\n(( total = 5 ))\nlet stepped=7\nfor (( index = 0; index < 3; index += 1 )); do\n  echo \"$index\"\ndone\necho \"$(( sum = 1 + 2 ))\"\necho \"$total$stepped$sum\"\n"}}}'
  frame '{"jsonrpc":"2.0","id":89,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/arithmetic-target.sh"}}}'
  frame '{"jsonrpc":"2.0","id":90,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/arithmetic-target.sh"},"position":{"line":7,"character":9}}}'
  frame '{"jsonrpc":"2.0","id":91,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/arithmetic-target.sh"},"position":{"line":7,"character":16}}}'
  frame '{"jsonrpc":"2.0","id":92,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/arithmetic-target.sh"},"position":{"line":7,"character":22}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/quoted-assignment.sh","languageId":"bash","version":1,"text":"#!/bin/bash\nexport \"quoted=value\"\ndeclare \"wrapped=$HOME\"\nreadonly '\''single=one'\''\nexport mixed\"tail=three\"\necho \"$quoted$wrapped$single$mixedtail\"\n"}}}'
  frame '{"jsonrpc":"2.0","id":93,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/quoted-assignment.sh"}}}'
  frame '{"jsonrpc":"2.0","id":94,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/quoted-assignment.sh"},"position":{"line":5,"character":9}}}'
  frame '{"jsonrpc":"2.0","id":95,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/quoted-assignment.sh"},"position":{"line":5,"character":16}}}'
  frame '{"jsonrpc":"2.0","id":96,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/quoted-assignment.sh"},"position":{"line":5,"character":24}}}'
  frame '{"jsonrpc":"2.0","id":97,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/quoted-assignment.sh"},"position":{"line":5,"character":32}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/shell-variable.sh","languageId":"bash","version":1,"text":"#!/bin/bash\necho \"$PATH $RANDOM $PIPESTATUS $KOSH_ANSI_RED $BASHOPTS $?\"\nHOME=/tmp\necho \"$HOME\"\n"}}}'
  frame '{"jsonrpc":"2.0","id":98,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/shell-variable.sh"},"position":{"line":1,"character":8}}}'
  frame '{"jsonrpc":"2.0","id":99,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/shell-variable.sh"},"position":{"line":1,"character":15}}}'
  frame '{"jsonrpc":"2.0","id":100,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/shell-variable.sh"},"position":{"line":1,"character":25}}}'
  frame '{"jsonrpc":"2.0","id":101,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/shell-variable.sh"},"position":{"line":1,"character":38}}}'
  frame '{"jsonrpc":"2.0","id":102,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/shell-variable.sh"},"position":{"line":1,"character":50}}}'
  frame '{"jsonrpc":"2.0","id":103,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/shell-variable.sh"},"position":{"line":1,"character":58}}}'
  frame '{"jsonrpc":"2.0","id":104,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/shell-variable.sh"},"position":{"line":3,"character":8}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/shell-variable-posix.sh","languageId":"sh","version":1,"text":"#!/bin/sh\necho \"$RANDOM\"\n"}}}'
  frame '{"jsonrpc":"2.0","id":105,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/shell-variable-posix.sh"},"position":{"line":1,"character":9}}}'
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
check_contains slash-function-definition '"id":70,"result":{"uri":"file:///tmp/slash-function.sh","range":{"start":{"line":1,"character":9},"end":{"line":1,"character":21}}}'
check_contains slash-function-hover '"id":71,"result":{"contents":{"kind":"plaintext","value":"ble/util/put () \n{\n  printf %s \"$1\"\n}"}'
check_contains slash-paren-definition '"id":72,"result":{"uri":"file:///tmp/slash-function.sh","range":{"start":{"line":5,"character":0},"end":{"line":5,"character":16}}}'
check_contains slash-path-stays-a-path '"id":73,"result":null'
check_contains document-symbol-capability '"documentSymbolProvider":true'
check_contains document-symbol-outline '"id":74,"result":[{"name":"top","kind":13,"range":{"start":{"line":1,"character":0},"end":{"line":1,"character":5}},"selectionRange":{"start":{"line":1,"character":0},"end":{"line":1,"character":3}}},{"name":"outer","kind":12,"range":{"start":{"line":2,"character":9},"end":{"line":5,"character":1}},"selectionRange":{"start":{"line":2,"character":9},"end":{"line":2,"character":14}},"children":[{"name":"inner","kind":13,"range":{"start":{"line":3,"character":8},"end":{"line":3,"character":15}},"selectionRange":{"start":{"line":3,"character":8},"end":{"line":3,"character":13}}}]},{"name":"helper","kind":12,"range":{"start":{"line":6,"character":0},"end":{"line":8,"character":1}},"selectionRange":{"start":{"line":6,"character":0},"end":{"line":6,"character":6}},"children":[{"name":"count","kind":13,"range":{"start":{"line":7,"character":2},"end":{"line":7,"character":9}},"selectionRange":{"start":{"line":7,"character":2},"end":{"line":7,"character":7}}}]}]'
check_contains document-symbol-unknown-document '"id":75,"result":[]'
check_contains hover-redirected-body '"id":76,"result":{"contents":{"kind":"plaintext","value":"quiet () \n{ echo hi; } >/dev/null 2>&1"}'
check_contains hover-closed-descriptor-body '"id":77,"result":{"contents":{"kind":"plaintext","value":"closed () \n{ :; } 2>&-"}'
check_contains declare-family-outline '"id":78,"result":[{"name":"plain","kind":13,"range":{"start":{"line":1,"character":8},"end":{"line":1,"character":15}},"selectionRange":{"start":{"line":1,"character":8},"end":{"line":1,"character":13}}},{"name":"shipped","kind":13,"range":{"start":{"line":2,"character":7},"end":{"line":2,"character":16}},"selectionRange":{"start":{"line":2,"character":7},"end":{"line":2,"character":14}}},{"name":"frozen","kind":13,"range":{"start":{"line":3,"character":9},"end":{"line":3,"character":21}},"selectionRange":{"start":{"line":3,"character":9},"end":{"line":3,"character":15}}},{"name":"listed","kind":13,"range":{"start":{"line":4,"character":11},"end":{"line":4,"character":18}},"selectionRange":{"start":{"line":4,"character":11},"end":{"line":4,"character":17}}},{"name":"slot","kind":13,"range":{"start":{"line":5,"character":8},"end":{"line":5,"character":17}},"selectionRange":{"start":{"line":5,"character":8},"end":{"line":5,"character":12}}},{"name":"use","kind":12,"range":{"start":{"line":6,"character":0},"end":{"line":9,"character":1}},"selectionRange":{"start":{"line":6,"character":0},"end":{"line":6,"character":3}},"children":[{"name":"scoped","kind":13,"range":{"start":{"line":7,"character":8},"end":{"line":7,"character":20}},"selectionRange":{"start":{"line":7,"character":8},"end":{"line":7,"character":14}}}]}]'
check_contains hover-local-assignment '"id":79,"result":{"contents":{"kind":"plaintext","value":"scoped=inner\nValue: inner\nThe assignment does not run on every path."}'
check_contains loop-binding-outline '"id":80,"result":[{"name":"entry","kind":13,"range":{"start":{"line":1,"character":4},"end":{"line":1,"character":9}},"selectionRange":{"start":{"line":1,"character":4},"end":{"line":1,"character":9}}},{"name":"choice","kind":13,"range":{"start":{"line":4,"character":7},"end":{"line":4,"character":13}},"selectionRange":{"start":{"line":4,"character":7},"end":{"line":4,"character":13}}}]'
check_contains hover-for-binding '"id":81,"result":{"contents":{"kind":"plaintext","value":"for entry in a b c; do\nThe name takes each word of the loop list in turn."}'
check_contains hover-select-binding '"id":82,"result":{"contents":{"kind":"plaintext","value":"select choice in one two; do\nThe name takes the menu entry the reader selects."}'
check_contains builtin-target-outline '"id":83,"result":[{"name":"field","kind":13,"range":{"start":{"line":1,"character":10},"end":{"line":1,"character":15}},"selectionRange":{"start":{"line":1,"character":10},"end":{"line":1,"character":15}}},{"name":"rows","kind":13,"range":{"start":{"line":2,"character":11},"end":{"line":2,"character":15}},"selectionRange":{"start":{"line":2,"character":11},"end":{"line":2,"character":15}}},{"name":"letter","kind":13,"range":{"start":{"line":3,"character":14},"end":{"line":3,"character":20}},"selectionRange":{"start":{"line":3,"character":14},"end":{"line":3,"character":20}}},{"name":"joined","kind":13,"range":{"start":{"line":4,"character":10},"end":{"line":4,"character":16}},"selectionRange":{"start":{"line":4,"character":10},"end":{"line":4,"character":16}}},{"name":"packed","kind":13,"range":{"start":{"line":5,"character":9},"end":{"line":5,"character":15}},"selectionRange":{"start":{"line":5,"character":9},"end":{"line":5,"character":15}}}]'
check_contains hover-read-binding '"id":84,"result":{"contents":{"kind":"plaintext","value":"read -n 5 field\nThe value is a field read from input."}'
check_contains hover-mapfile-binding '"id":85,"result":{"contents":{"kind":"plaintext","value":"mapfile -t rows < listing\nThe value is a list of lines read from input."}'
check_contains hover-getopts-binding '"id":86,"result":{"contents":{"kind":"plaintext","value":"getopts \"ab:\" letter\nThe value is the option letter the parse reached."}'
check_contains hover-printf-binding '"id":87,"result":{"contents":{"kind":"plaintext","value":"printf -v joined \"%s\" x\nThe value is the formatted text."}'
check_contains hover-printf-attached-binding '"id":88,"result":{"contents":{"kind":"plaintext","value":"printf -vpacked \"%s\" y\nThe value is the formatted text."}'
check_contains arithmetic-target-outline '"id":89,"result":[{"name":"total","kind":13,"range":{"start":{"line":1,"character":3},"end":{"line":1,"character":8}},"selectionRange":{"start":{"line":1,"character":3},"end":{"line":1,"character":8}}},{"name":"stepped","kind":13,"range":{"start":{"line":2,"character":4},"end":{"line":2,"character":11}},"selectionRange":{"start":{"line":2,"character":4},"end":{"line":2,"character":11}}},{"name":"index","kind":13,"range":{"start":{"line":3,"character":7},"end":{"line":3,"character":12}},"selectionRange":{"start":{"line":3,"character":7},"end":{"line":3,"character":12}}},{"name":"sum","kind":13,"range":{"start":{"line":6,"character":10},"end":{"line":6,"character":13}},"selectionRange":{"start":{"line":6,"character":10},"end":{"line":6,"character":13}}}]'
check_contains hover-arithmetic-command '"id":90,"result":{"contents":{"kind":"plaintext","value":"(( total = 5 ))\nThe value is the result of an arithmetic expression."}'
check_contains hover-let-binding '"id":91,"result":{"contents":{"kind":"plaintext","value":"let stepped=7\nThe value is the result of an arithmetic expression."}'
check_contains hover-arithmetic-expansion '"id":92,"result":{"contents":{"kind":"plaintext","value":"echo \"$(( sum = 1 + 2 ))\"\nThe value is the result of an arithmetic expression."}'
check_contains quoted-assignment-outline '"id":93,"result":[{"name":"quoted","kind":13,"range":{"start":{"line":1,"character":7},"end":{"line":1,"character":21}},"selectionRange":{"start":{"line":1,"character":7},"end":{"line":1,"character":21}}},{"name":"wrapped","kind":13,"range":{"start":{"line":2,"character":8},"end":{"line":2,"character":23}},"selectionRange":{"start":{"line":2,"character":8},"end":{"line":2,"character":23}}},{"name":"single","kind":13,"range":{"start":{"line":3,"character":9},"end":{"line":3,"character":21}},"selectionRange":{"start":{"line":3,"character":9},"end":{"line":3,"character":21}}},{"name":"mixedtail","kind":13,"range":{"start":{"line":4,"character":7},"end":{"line":4,"character":24}},"selectionRange":{"start":{"line":4,"character":7},"end":{"line":4,"character":24}}}]'
check_contains hover-quoted-assignment '"id":94,"result":{"contents":{"kind":"plaintext","value":"\"quoted=value\"\nValue: value"}'
check_contains hover-quoted-expansion '"id":95,"result":{"contents":{"kind":"plaintext","value":"\"wrapped=$HOME\"\nThe value is known only at run time."}'
check_contains hover-single-quoted-assignment '"id":96,"result":{"contents":{"kind":"plaintext","value":"'\''single=one'\''\nValue: one"}'
check_contains hover-mixed-quoted-assignment '"id":97,"result":{"contents":{"kind":"plaintext","value":"mixed\"tail=three\"\nValue: three"}'
check_contains hover-shell-variable '"id":98,"result":{"contents":{"kind":"plaintext","value":"PATH\nThe value is a colon-separated command search path. A restricted shell refuses to change it."}'
check_contains hover-dynamic-variable '"id":99,"result":{"contents":{"kind":"plaintext","value":"RANDOM\nEach read supplies a new random number between 0 and 32767.\nThe shell computes the value on each read.\nThe name is unavailable in the sh mood."}'
check_contains hover-array-variable '"id":100,"result":{"contents":{"kind":"plaintext","value":"PIPESTATUS\nThe elements are the exit status of every stage of the last pipeline.\nThe value is a list.\nThe name is defined by bash and not by POSIX."}'
check_contains hover-color-variable '"id":101,"result":{"contents":{"kind":"plaintext","value":"KOSH_ANSI_RED\nThe value is a terminal escape, and it is empty when color is disabled.\nThe shell computes the value on each read."}'
check_contains hover-unmaintained-variable '"id":102,"result":{"contents":{"kind":"plaintext","value":"BASHOPTS\nThe value is a colon-separated list of the shopt options that are on in bash.\nThis shell does not maintain the name.\nThe name is defined by bash and not by POSIX."}'
check_contains hover-special-parameter '"id":103,"result":{"contents":{"kind":"plaintext","value":"$?\nThe value is the exit status of the last command.\nThe shell computes the value on each read."}'
check_contains hover-assigned-shell-variable '"id":104,"result":{"contents":{"kind":"plaintext","value":"HOME=/tmp\nValue: /tmp\n\nThe shell also defines the name.\nThe value is the home directory used by tilde expansion and by a bare cd."}'
check_contains hover-posix-mood-variable '"id":105,"result":{"contents":{"kind":"plaintext","value":"RANDOM\nEach read supplies a new random number between 0 and 32767.\nThe shell computes the value on each read.\nThe sh mood is active, so the name is unavailable."}'
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

unterminated_output=$(
  {
    frame '{"jsonrpc":"2.0","id":60,"method":"initialize","params":{"capabilities":{}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/unterminated.sh","languageId":"bash","version":1,"text":"x=1\nfunction f { printf '\''oops ; }\n"}}}'
    frame '{"jsonrpc":"2.0","id":61,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/unterminated.sh"},"position":{"line":1,"character":10}}}'
    frame '{"jsonrpc":"2.0","id":62,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///tmp/unterminated.sh"}}}'
    frame '{"jsonrpc":"2.0","id":63,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit"}'
  } | "$BIN" --language-server
)
unterminated_status=$?
printf 'unterminated-status=%s\n' "$unterminated_status"
case $unterminated_output in
*'"message":"Unterminated string literal"'*'"id":61,"result":{"uri"'*'"id":62,"result":{"data":['*)
  printf 'unterminated-survives=ok\n'
  ;;
*) printf 'unterminated-survives=missing\n' ;;
esac

shift_output=$(
  {
    frame '{"jsonrpc":"2.0","id":70,"method":"initialize","params":{"capabilities":{}}}'
    frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/shift.sh","languageId":"bash","version":1,"text":"mapfile -t v <<< \"$1\"\n((w<<=1))\ngreet() { :; }\ngreet\n"}}}'
    frame '{"jsonrpc":"2.0","id":71,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/shift.sh"},"position":{"line":3,"character":2}}}'
    frame '{"jsonrpc":"2.0","id":72,"method":"shutdown","params":null}'
    frame '{"jsonrpc":"2.0","method":"exit"}'
  } | "$BIN" --language-server
)
case $shift_output in
*'"id":71,"result":{"uri":"file:///tmp/shift.sh","range":{"start":{"line":2,'*)
  printf 'shift-not-heredoc=ok\n'
  ;;
*) printf 'shift-not-heredoc=missing\n' ;;
esac

frame '{"jsonrpc":"2.0","method":"exit"}' |
  "$BIN" --language-server > /dev/null
printf 'exit-without-shutdown-status=%s\n' "$?"
