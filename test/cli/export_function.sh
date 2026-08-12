# shellcheck disable=SC2154
# export -f serializes a shell function into the environment under the bash
# BASH_FUNC_name%% convention so a child bash inherits it. kosh does not import
# BASH_FUNC itself, so an inherited entry stays inert and cannot inject code.
echo "== brace-body function serializes:"
"$BIN" -c 'greet() { echo "hi $1"; }; export -f greet; env' 2>/dev/null | grep BASH_FUNC_greet
echo "== error on a non-function:"
"$BIN" -c 'export -f not_a_function' 2>&1
echo "== an inherited BASH_FUNC is not imported and cannot inject:"
env 'BASH_FUNC_x%%=() { echo body; }; echo INJECTED' "$BIN" -c \
    'type x || echo x-absent' 2>/dev/null
