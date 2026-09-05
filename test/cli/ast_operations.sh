printf 'echo   format\n' | "$BIN" -A --format
printf 'echo lint\n' | "$BIN" -A --lint --no-traces
printf 'echo   combined\n' | "$BIN" -A --lint --format --no-traces

root=$TEST_TEMP_DIRECTORY/ast-operations
mkdir -p "$root"
trap '[ -n "$root" ] && /bin/rm -rf "$root"' EXIT

printf 'echo apply\n' > "$root/lint.sh"
"$BIN" -A --lint --apply --no-traces "$root/lint.sh"
printf 'echo both\n' > "$root/both.sh"
"$BIN" -A --lint --format --apply --no-traces "$root/both.sh"
printf '# heading\n\ntext\n\n```sh\necho   embedded\n```\n' > "$root/plain.md"
"$BIN" -A --format "$root/plain.md"
"$BIN" -A --lint --no-traces "$root/plain.md"
