unset KOSH_FLAGS
# A login shell receives a dash-prefixed argv[0], a bare - or -kosh, the spawn
# convention login and tmux use. SHELL and BASH must name a file a child can run,
# so the login dash is dropped from them, while $0 keeps the dashed spelling
# verbatim the way bash does. The login shell is built through a dash-prefixed
# symlink the way login starts one, and the output is filtered to the two markers
# so a sourced profile cannot perturb it.
echo "== SHELL is runnable while \$0 keeps the login dash:"
"$BIN" -c 'exec -a -kosh "$1" -c '\''
  case "$SHELL" in -*) echo "SHELL=dashed" ;; *) echo "SHELL=runnable" ;; esac
  case "$0" in -*) echo "zero=dashed" ;; *) echo "zero=runnable" ;; esac
'\''' test-login "$BIN" 2>/dev/null | grep -E "^(SHELL|zero)="
