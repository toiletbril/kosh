unset KOSH_FLAGS
start=$PWD
dir=$(mktemp -d)
trap 'cd "$start" && rm -rf "$dir"' EXIT
cd "$dir"
printf 'set -u\necho "$undefined_variable"\necho after\n' > setu.sh
printf 'echo "${missing:?is gone}"\necho after\n' > report.sh
echo "== a bash-mood set -u abort exits 1 like a bash script, not 127 =="
"$BIN" --mood bash setu.sh
echo "rc=$?"
echo "== a bash-mood \${name:?} abort exits 1 like a bash script =="
"$BIN" --mood bash report.sh
echo "rc=$?"
