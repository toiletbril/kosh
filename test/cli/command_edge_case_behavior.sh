# Regression tests for correctness fixes found in the code-review sweep.
unset KOSH_FLAGS
echo "== bare wait returns 0, not the last job status:"
"$BIN" -c '(exit 7) & wait; echo "rc=$?"'
echo "== declare -x exports an already-set variable:"
"$BIN" -c 'x=hello; declare -x x; env' 2>/dev/null | grep '^x=hello'
echo "== an unquoted heredoc leaves a dollar-single-quote literal:"
"$BIN" -s <<'OUTER'
cat <<EOF
$'\n'
EOF
OUTER
echo "== pkill rejects an empty pattern:"
"$BIN" -c 'koshkit pkill ""' 2>&1
echo "== cp -r of a directory into itself is refused, not infinite:"
start=$PWD
d=$(mktemp -d)
trap 'cd "$start" && rm -rf "$d"' EXIT
cd "$d" || exit 1
mkdir sub
: > sub/f
"$BIN" -c 'koshkit cp -r sub sub' 2>&1
