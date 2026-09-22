unset KOSH_FLAGS

echo "== export completes variable names without an equals suffix:"
completion=$(\
  KOSH_EXPORT_COMPLETION_UNIQUE=1 \
    "$BIN" --debug-complete-at 'export KOSH_EXPORT_COMPLETION_UNI' </dev/null
)
printf '%s\n' "$completion"
if printf '%s\n' "$completion" | grep -Fx \
  'KOSH_EXPORT_COMPLETION_UNIQUE' >/dev/null &&
  ! printf '%s\n' "$completion" | grep -F '=' >/dev/null
then
  echo "export-candidate=plain"
else
  echo "export-candidate=wrong"
fi
