unset KOSH_FLAGS
# An executed script roots BASH_SOURCE at the script itself, so the envman
# style probe test "$BASH_SOURCE" == "$0" takes its executed branch under
# mimicry, while a dot-sourced run still reports the sourcing.
script=$(mktemp)
cat > "$script" <<'EOF'
#!/bin/bash
echo "match=$([ "${BASH_SOURCE:-}" = "$0" ] && echo yes || echo no)"
echo "scalar_empty=$([ -z "${BASH_SOURCE:-}" ] && echo yes || echo no)"
EOF
chmod +x "$script"
"$BIN" -I -c "$script"
"$BIN" -I -c ". $script" | sed -n '1p'
rm -f "$script"
echo "rc=$?"
