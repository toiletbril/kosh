unset KOSH_FLAGS
# A login spawn hands the shell a dash-prefixed argv[0], the way tmux and
# login start -bash, so the name is never read as a flag bundle and the
# session comes up as a login shell instead of dying on Unknown flag '-b'.
"$BIN" -c 'exec -a -kosh "$1" -c "echo login_spawn_ok"' \
    test-login "$BIN" 2>/dev/null | grep -c login_spawn_ok
echo "rc=$?"
# exec -l prepends a dash to the whole argv[0], so a path invocation such as
# -/usr/local/bin/kosh still marks a login shell. The dash is stripped from
# $KOSH while $0 keeps the dashed spelling.
out=$("$BIN" -c 'exec -l "$1" -c '\''printf "%s\n" "$KOSH"'\''' \
    test-login "$BIN" 2>/dev/null)
case "$out" in
  -*) echo "shle_leading_dash=yes" ;;
  *)  echo "shle_leading_dash=no" ;;
esac
