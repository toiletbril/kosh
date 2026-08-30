directory=$(mktemp -d) || exit 1
trap '[ -n "$directory" ] && /bin/rm -rf "$directory"' EXIT

frame()
{
  body=$1
  printf 'Content-Length: %s\r\n\r\n%s' "${#body}" "$body"
}

{
  frame '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"rootUri":"file:///tmp","capabilities":{"general":{"positionEncodings":["utf-8"]},"textDocument":{"publishDiagnostics":{"dataSupport":true},"codeAction":{"isPreferredSupport":true,"codeActionLiteralSupport":{"codeActionKind":{"valueSet":["quickfix","source.fixAll.kosh"]}}}}}}}'
  frame '{"jsonrpc":"2.0","method":"initialized","params":{}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml","languageId":"yaml","version":1,"text":"name: $HOST_TEXT\njobs:\n  check:\n    steps:\n      - run: |\n          value=ready\n          printf \"%s\\n\" \"$value\" \"$EMBEDDED_TEXT\"\n"}}}'
  frame '{"jsonrpc":"2.0","id":2,"method":"textDocument/hover","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml"},"position":{"line":6,"character":27}}}'
  frame '{"jsonrpc":"2.0","id":3,"method":"textDocument/definition","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml"},"position":{"line":6,"character":27}}}'
  frame '{"jsonrpc":"2.0","id":4,"method":"textDocument/documentSymbol","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml"}}}'
  frame '{"jsonrpc":"2.0","id":5,"method":"textDocument/semanticTokens/full","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml"}}}'
  frame '{"jsonrpc":"2.0","id":6,"method":"textDocument/formatting","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml"},"options":{"tabSize":2,"insertSpaces":true}}}'
  frame '{"jsonrpc":"2.0","id":8,"method":"textDocument/completion","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml"},"position":{"line":6,"character":13}}}'
  frame '{"jsonrpc":"2.0","id":9,"method":"textDocument/rename","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml"},"position":{"line":6,"character":27},"newName":"renamed"}}'
  frame '{"jsonrpc":"2.0","id":10,"method":"textDocument/codeAction","params":{"textDocument":{"uri":"file:///tmp/.github/workflows/check.yml"},"range":{"start":{"line":6,"character":36},"end":{"line":6,"character":50}},"context":{"diagnostics":[]}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/README.md","languageId":"markdown","version":1,"text":"```bash\nmissing_lsp_executable\n```\n"}}}'
  frame '{"jsonrpc":"2.0","method":"textDocument/didOpen","params":{"textDocument":{"uri":"file:///tmp/.devcontainer/devcontainer.json","languageId":"json","version":1,"text":"{\n\"postCreateCommand\":\"missing_lsp_dev_target\",\n\"waitFor\":\"missing_lsp_dev_wait\",\n\"initializeCommand\":\"missing_lsp_dev_host\"\n}"}}}'
  frame '{"jsonrpc":"2.0","id":7,"method":"shutdown","params":null}'
  frame '{"jsonrpc":"2.0","method":"exit"}'
} > "$directory/input"

output=$("$BIN" --language-server < "$directory/input")
status=$?
printf 'status=%s\n' "$status"

case $output in
*"The variable 'EMBEDDED_TEXT'"*) printf 'embedded-diagnostic=ok\n' ;;
*) printf 'embedded-diagnostic=missing\n' ;;
esac
case $output in
*"The variable 'HOST_TEXT'"*) printf 'host-diagnostic=unexpected\n' ;;
*) printf 'host-diagnostic=none\n' ;;
esac
case $output in
*'missing-shebang'*) printf 'host-shebang=unexpected\n' ;;
*) printf 'host-shebang=none\n' ;;
esac
case $output in
*"The command 'missing_lsp_executable' was not found"*)
  printf 'local-command-diagnostic=ok\n'
  ;;
*) printf 'local-command-diagnostic=missing\n' ;;
esac
case $output in
*'"start":{"line":3,"character":21},"end":{"line":3,"character":41}'*"The command 'missing_lsp_dev_host' was not found"*)
  printf 'dev-host-diagnostic=ok\n'
  ;;
*) printf 'dev-host-diagnostic=missing\n' ;;
esac
case $output in
*'missing_lsp_dev_target'*) printf 'dev-target-diagnostic=unexpected\n' ;;
*) printf 'dev-target-diagnostic=none\n' ;;
esac
case $output in
*'missing_lsp_dev_wait'*) printf 'dev-wait-diagnostic=unexpected\n' ;;
*) printf 'dev-wait-diagnostic=none\n' ;;
esac
case $output in
*'"name":"value"'*) printf 'outline=ok\n' ;;
*) printf 'outline=missing\n' ;;
esac
case $output in
*'"id":5,"result":{"data":['*']}'*) printf 'semantic-tokens=ok\n' ;;
*) printf 'semantic-tokens=missing\n' ;;
esac
case $output in
*'"id":6,"result":'*) printf 'format-preserves-host=ok\n' ;;
*) printf 'format-preserves-host=missing\n' ;;
esac
case $output in
*'"id":2,"result":{"contents":'*) printf 'hover=ok\n' ;;
*) printf 'hover=missing\n' ;;
esac
case $output in
*'"id":3,"result":{"uri":"file:///tmp/.github/workflows/check.yml"'*) printf 'definition=ok\n' ;;
*) printf 'definition=missing\n' ;;
esac
case $output in
*'"id":8,"result":['*'"label":"printf"'*) printf 'completion=ok\n' ;;
*) printf 'completion=missing\n' ;;
esac
case $output in
*'"id":9,"result":'*'"newText":"renamed"'*) printf 'rename=ok\n' ;;
*) printf 'rename=missing\n' ;;
esac
case $output in
*'"id":10,"result":['*) printf 'code-actions=ok\n' ;;
*) printf 'code-actions=missing\n' ;;
esac
