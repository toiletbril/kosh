dir=$(mktemp -d)
trap '[ -n "$dir" ] && /bin/rm -rf "$dir"' EXIT

script="$dir/named.kosh"
{
    printf 'if true; then\r\n'
    printf '  echo named\r\n'
    printf 'fi\r\n'
    printf 'printf "joined=%%s\\n" foo\\\r\n'
    printf 'bar\r\n'
    printf 'value="alpha\r\nomega"\r\n'
    printf 'printf "quoted=%%s\\n" "${#value}"\r\n'
    printf 'value=$(cat <<EOF\r\n'
    printf 'body\r\n'
    printf 'EOF\r\n'
    printf ')\r\n'
    printf 'printf "heredoc=%%s:%%s\\n" "${#value}" "$value"\r\n'
} > "$script"
"$BIN" "$script"

printf 'echo stdin\r\n' | "$BIN" -s

source_script="$dir/source.kosh"
printf 'echo sourced\r\n' > "$source_script"
"$BIN" -c '. "$1"' crlf-driver "$source_script"

eval_text=$(printf 'echo eval-one\r\necho eval-two\r\n_')
eval_text=${eval_text%_}
"$BIN" -c 'eval "$1"' crlf-driver "$eval_text"

fallback_script="$dir/fallback.ps1"
printf 'echo fallback\r\n' > "$fallback_script"
/bin/chmod +x "$fallback_script"
"$BIN" -c '"$1"' crlf-driver "$fallback_script"

lone_carriage_return=$(printf 'a\rb')
"$BIN" -c 'value=$1; printf "lone=%s\n" "${#value}"' crlf-driver \
    "$lone_carriage_return"

execution_string=$(printf '[ "$BASH_EXECUTION_STRING" = "$1" ] || exit 1\r\necho execution-string=exact\r\n_')
execution_string=${execution_string%_}
"$BIN" --mood bash -c "$execution_string" crlf-driver "$execution_string"

invalid_script="$dir/invalid.kosh"
printf 'echo first\r\nmissing_crlf_probe\r\n' > "$invalid_script"
diagnostic=$("$BIN" "$invalid_script" 2>&1) && exit 1
case "$diagnostic" in
    *:2:1:*) ;;
    *) exit 1 ;;
esac
echo 'diagnostic=clean'
