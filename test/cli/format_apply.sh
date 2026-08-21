unset KOSH_FLAGS
root=$TEST_TEMP_DIRECTORY/format-apply
mkdir -p "$root"
trap '[ -n "$root" ] && /bin/rm -rf "$root"' EXIT

help_output=$("$BIN" --help 2>&1)
printf 'help-status=%s\n' "$?"
case $help_output in
*'AUXILIARY OPTIONS'*'--lint'*'--format'*'--apply'*'--language-server'*)
  printf 'help-auxiliary=yes\n'
  ;;
*) printf 'help-auxiliary=no\n' ;;
esac

printf 'if test -f "\044x"; then echo yes; else echo no; fi\n' |
  "$BIN" --format

cat > "$root/comments.sh" <<'EOF'
#!/bin/sh
  # keep this text without wrapping
if true; then echo x # preserve this inline comment
       # this standalone comment stays longer than eighty columns without being wrapped or shortened
cat <<-BODY
	body $x
	BODY
fi
EOF
"$BIN" --format "$root/comments.sh"

cat > "$root/shadow.sh" <<'EOF'
test -n "$x"
test() { echo shadow; }
test -n "$x"
command test -n "$x"
EOF
"$BIN" --format "$root/shadow.sh"

nested=$(printf 'value=\044(if test -n "\0441"; then echo yes; else echo no; fi)\n' |
  "$BIN" --format)
printf '%s\n' "$nested"
second=$(printf '%s\n' "$nested" | "$BIN" --format)
printf 'idempotent=%s\n' "$([ "$nested" = "$second" ] && printf yes)"
wrapped=$(printf 'printf one two three four five six seven eight nine ten eleven twelve thirteen fourteen fifteen sixteen\n' |
  "$BIN" --format)
printf '%s\n' "$wrapped"
wrapped_second=$(printf '%s\n' "$wrapped" | "$BIN" --format)
maximum_width=0
while IFS= read -r line; do
  if [ "${#line}" -gt "$maximum_width" ]; then
    maximum_width=${#line}
  fi
done <<< "$wrapped"
printf 'wrap-idempotent=%s maximum=%s\n' \
  "$([ "$wrapped" = "$wrapped_second" ] && printf yes)" "$maximum_width"
short_options='ffmpeg value value --some flag value --some flag -abced --flag as'
printf '%s\n' "$short_options" | "$BIN" --format
long_options='ffmpeg first-positional second-positional'
long_options="$long_options --some flag value --other flag value"
long_options="$long_options -abced --flag as trailing-positional-value"
wrapped_options=$(printf '%s\n' "$long_options" | "$BIN" --format)
printf '%s\n' "$wrapped_options"
wrapped_options_second=$(printf '%s\n' "$wrapped_options" | "$BIN" --format)
printf 'option-wrap-idempotent=%s\n' \
  "$([ "$wrapped_options" = "$wrapped_options_second" ] && printf yes)"
printf 'echo {a,b}\nitems=(one two)\nfor ((i=0; i<2; i++)); do echo "\044i"; done\necho @(a|b)\n' |
  "$BIN" --format
printf 'printf "%%s\\n" if test x = x\nprintf "%%s\\n" ! test x = y\ncase x in test) echo yes;; esac\ntest -n "\044x" # keep\ntest -f x 2>/dev/null\n2>/dev/null test -n x\nprintf err >&2\nexec 3>&1\n(echo sub)\n' |
  "$BIN" --format
printf 'if [[ $exits > 0 ]]; then exit 1; fi\n' | "$BIN" --format
wrapped_again=$(printf '%s\n' "$nested" | "$BIN" --format)
printf 'nested-idempotent=%s\n' \
  "$([ "$nested" = "$wrapped_again" ] && printf yes)"
printf 'if cat <<EOF; then\nbody\nEOF\necho done\nfi\n' |
  "$BIN" --format
printf 'if cat <<EOF; then # keep\nbody\nEOF\necho done\nfi\n' |
  "$BIN" --format
printf 'printf then <<EOF\nbody\nEOF\n' | "$BIN" --format
printf 'if { true; } && { false; } || true; then :; fi || false\ncase x in x) :;; esac | cat\n' |
  "$BIN" --format
printf 'case x in\nx) echo arm\nesac\necho top\n' | "$BIN" --format
printf 'if true; then echo dash; fi\n' | "$BIN" --format -
printf '{ echo brace; } >&2\n( echo sub ) >&2\nfor i in 1; do echo "\044i"; done >/dev/null\nwhile false; do :; done 2>/dev/null\ncase x in x) :;; esac >/dev/null 2>&1\nif true; then echo cond; fi >/dev/null\n{ echo both; } >/dev/null 2>&1\n' |
  "$BIN" --format

cat > "$root/compound-redirect.sh" <<'EOF'
target=$1
{ echo grouped; } > "$target"
for word in one two; do echo "$word"; done >> "$target"
while [ -n "$target" ]; do target=; done 2>/dev/null
case $1 in *) echo arm ;; esac >> "$1"
if true; then echo branch; fi >> "$1"
cat "$1"
EOF
"$BIN" --format "$root/compound-redirect.sh" \
  > "$root/compound-redirect-formatted.sh"
compound_original=$("$BIN" "$root/compound-redirect.sh" "$root/compound-one.txt")
compound_formatted=$("$BIN" "$root/compound-redirect-formatted.sh" \
  "$root/compound-two.txt")
printf 'compound-redirect-equivalent=%s captured=%s\n' \
  "$([ "$compound_original" = "$compound_formatted" ] && printf yes)" \
  "$(cmp -s "$root/compound-one.txt" "$root/compound-two.txt" && printf yes)"

printf 'set -- foo\\\nbar\nprintf "<%%s> count=%%s\\n" "\0441" "\044#"\n' \
  > "$root/continuation.sh"
"$BIN" --format "$root/continuation.sh" > "$root/continuation-formatted.sh"
continuation_original=$("$BIN" "$root/continuation.sh")
continuation_formatted=$("$BIN" "$root/continuation-formatted.sh")
"$BIN" --format "$root/continuation-formatted.sh" \
  > "$root/continuation-second.sh"
printf 'continuation-equivalent=%s idempotent=%s\n' \
  "$([ "$continuation_original" = "$continuation_formatted" ] && printf yes)" \
  "$(cmp -s "$root/continuation-formatted.sh" \
      "$root/continuation-second.sh" && printf yes)"

printf 'printf a\rb\n' > "$root/lone-cr.sh"
cp "$root/lone-cr.sh" "$root/lone-cr.expected"
"$BIN" --format "$root/lone-cr.sh" > "$root/lone-cr-formatted.sh"
"$BIN" --format "$root/lone-cr-formatted.sh" > "$root/lone-cr-second.sh"
printf 'lone-cr-retained=%s idempotent=%s\n' \
  "$(cmp -s "$root/lone-cr.expected" "$root/lone-cr-formatted.sh" && \
      printf yes)" \
  "$(cmp -s "$root/lone-cr-formatted.sh" "$root/lone-cr-second.sh" && \
      printf yes)"

cat > "$root/assignment-prefixes.sh" <<'EOF'
x+=1 test -n "$x"
arr[0]=v test -n "${arr[0]}"
arr[1]+=w test -n "${arr[1]}"
EOF
"$BIN" --format "$root/assignment-prefixes.sh" \
  > "$root/assignment-prefixes-formatted.sh"
cat "$root/assignment-prefixes-formatted.sh"
"$BIN" --format "$root/assignment-prefixes-formatted.sh" \
  > "$root/assignment-prefixes-second.sh"
printf 'assignment-prefixes-idempotent=%s\n' \
  "$(cmp -s "$root/assignment-prefixes-formatted.sh" \
      "$root/assignment-prefixes-second.sh" && printf yes)"

cat > "$root/nested-heredoc.sh" <<'SCRIPT'
one=$(cat <<'ONE'
) " } # ; $(
ONE
)
two=$(cat <<-TWO
	) unquoted
	TWO
)
three=$(cat <<LEFT <<'RIGHT'
ignored )
LEFT
right " )
RIGHT
)
printf '<%s>|<%s>|<%s>\n' "$one" "$two" "$three"
SCRIPT
"$BIN" --format "$root/nested-heredoc.sh" \
  > "$root/nested-heredoc-formatted.sh"
nested_heredoc_original=$("$BIN" "$root/nested-heredoc.sh")
nested_heredoc_formatted=$("$BIN" "$root/nested-heredoc-formatted.sh")
"$BIN" --format "$root/nested-heredoc-formatted.sh" \
  > "$root/nested-heredoc-second.sh"
printf 'nested-heredoc-equivalent=%s idempotent=%s\n' \
  "$([ "$nested_heredoc_original" = "$nested_heredoc_formatted" ] && \
      printf yes)" \
  "$(cmp -s "$root/nested-heredoc-formatted.sh" \
      "$root/nested-heredoc-second.sh" && printf yes)"

cat > "$root/first.sh" <<'EOF'
for x in a b; do test "$x" = a && echo "$x"; done
EOF
cat > "$root/second.sh" <<'EOF'
if true; then echo second; fi
EOF
chmod 755 "$root/first.sh"
ln -s first.sh "$root/link.sh"
ln "$root/second.sh" "$root/second-hardlink.sh"
"$BIN" --format --apply "$root/link.sh" "$root/second.sh"
apply_status=$?
if [ "${OS-}" = Windows_NT ] || [ -x "$root/first.sh" ]; then
  is_executable=yes
else
  is_executable=
fi
printf 'apply-status=%s executable=%s symlink=%s\n' "$apply_status" \
  "$is_executable" \
  "$([ -L "$root/link.sh" ] && printf yes)"
cat "$root/first.sh" "$root/second.sh"
printf 'hardlink-retained=%s\n' \
  "$([ "$(cat "$root/second-hardlink.sh")" = 'if true; then echo second; fi' ] && printf yes)"

printf '#!/bin/sh\r\n[ "\0441" = y ]\r\n' > "$root/clean-crlf.sh"
cp "$root/clean-crlf.sh" "$root/clean-crlf.expected"
"$BIN" --lint --apply --no-traces "$root/clean-crlf.sh" >/dev/null 2>&1
printf 'clean-crlf-status=%s retained=%s\n' "$?" \
  "$(cmp -s "$root/clean-crlf.sh" "$root/clean-crlf.expected" && printf yes || printf no)"
printf '#!/bin/sh\r\n[ "\0441" == y ]\r\n' > "$root/fixed-crlf.sh"
printf '#!/bin/sh\r\n[ "\0441" = y ]\r\n' > "$root/fixed-crlf.expected"
"$BIN" --lint --apply --no-traces "$root/fixed-crlf.sh" >/dev/null 2>&1
printf 'fixed-crlf-status=%s retained=%s\n' "$?" \
  "$(cmp -s "$root/fixed-crlf.sh" "$root/fixed-crlf.expected" && printf yes || printf no)"

printf '\357\273\277#!/bin/sh\n[ "\0441" == y ]\nprintf "%%s\\n" \0441 \044@\n' > "$root/fix.sh"
"$BIN" --lint --apply --no-traces "$root/fix.sh" >/dev/null 2>&1
printf 'lint-apply-status=%s contents=%s\n' "$?" \
  "$(tr '\n' '|' < "$root/fix.sh")"

printf '#!/bin/bash\nif test -n "\0441"; then [ "\0441" == y ]; fi\nif [[ \0441 > 0 ]]; then echo positive; fi\n' > "$root/both.sh"
"$BIN" --lint --format --apply --no-traces "$root/both.sh" >/dev/null 2>&1
printf 'combined-status=%s\n' "$?"
cat "$root/both.sh"

printf '#!/bin/bash\n[[ 1 > 0 ]]\n' > "$root/warning.sh"
"$BIN" --lint --format --apply --no-traces "$root/warning.sh" >/dev/null 2>&1
printf 'warning-apply-status=%s\n' "$?"
cat "$root/warning.sh"

printf '#!/bin/sh\n[ "\0441" == y ]\nprintf "%%s\\n" \0441\n' \
  > "$root/safe-and-unsafe.sh"
"$BIN" --lint --apply --no-traces "$root/safe-and-unsafe.sh" \
  >/dev/null 2>&1
printf 'safe-and-unsafe-status=%s contents=%s\n' "$?" \
  "$(tr '\n' '|' < "$root/safe-and-unsafe.sh")"

printf '\357\273\277#!/bin/sh\n[ "\0441" == y ]\n[ "\0442" == n ]\nprintf "%%s\\n" \0441\n' \
  > "$root/summary.sh"
"$BIN" --lint --apply --no-traces "$root/summary.sh" \
  > /dev/null 2> "$root/summary.err"
printf 'summary-status=%s\n' "$?"
cat "$root/summary.err"

cat > "$root/safe-fixes.sh" <<'EOF'
#!/bin/sh
value=yes
test "x$value" = xyes && echo one
[ x"$value" = "xyes" ] && echo two
[ ! -z "$value" ] && echo three
[ ! -n "$value" ] || echo four
EOF
safe_fixes_original=$("$BIN" --no-diagnostics "$root/safe-fixes.sh")
"$BIN" --lint --apply --no-traces "$root/safe-fixes.sh" \
  > /dev/null 2> "$root/safe-fixes.err"
printf 'safe-fixes-status=%s\n' "$?"
cat "$root/safe-fixes.err"
safe_fixes_applied=$("$BIN" --no-diagnostics "$root/safe-fixes.sh")
printf 'safe-fixes-equivalent=%s\n' \
  "$([ "$safe_fixes_original" = "$safe_fixes_applied" ] && printf yes)"
cat "$root/safe-fixes.sh"

printf '#!/bin/sh\negrep pattern /dev/null\nfgrep pattern /dev/null\n' \
  > "$root/skipped-fixes.sh"
printf 'echo \044[1+2]\n[ x\0441 = xbare ] && echo unquoted\necho \044[v[0]+1]\n' \
  >> "$root/skipped-fixes.sh"
"$BIN" --lint --apply --no-traces "$root/skipped-fixes.sh" \
  > /dev/null 2> "$root/skipped-fixes.err"
printf 'skipped-fixes-status=%s contents=%s\n' "$?" \
  "$(tr '\n' '|' < "$root/skipped-fixes.sh")"
cat "$root/skipped-fixes.err"

cat > "$root/shapes.sh" <<'EOF'
items=(one two three)
flag=on left=right other=
count=${#items[@]}
if [[ $flag && ( $left == right || $other ) ]]; then echo cond; fi
case $1 in
(alpha) echo first ;;
(beta|gamma) echo second ;;
*) echo "rest $count" ;;
esac
for name in one two; do echo "${#name}"; done
EOF
"$BIN" --format "$root/shapes.sh" > "$root/shapes-formatted.sh"
cat "$root/shapes-formatted.sh"
"$BIN" --format "$root/shapes-formatted.sh" > "$root/shapes-second.sh"
shapes_original=$("$BIN" --no-diagnostics "$root/shapes.sh" alpha)
shapes_formatted=$("$BIN" --no-diagnostics "$root/shapes-formatted.sh" alpha)
printf 'shapes-equivalent=%s idempotent=%s\n' \
  "$([ "$shapes_original" = "$shapes_formatted" ] && printf yes)" \
  "$(cmp -s "$root/shapes-formatted.sh" "$root/shapes-second.sh" && \
      printf yes)"

printf 'builtin eval -- "\044(one/two/three.dump | four/five:six/seven.awk resolve 2>/dev/null)"\n' \
  > "$root/substitution-wrap.sh"
"$BIN" --format "$root/substitution-wrap.sh" \
  > "$root/substitution-wrap-formatted.sh"
cat "$root/substitution-wrap-formatted.sh"
"$BIN" --format "$root/substitution-wrap-formatted.sh" \
  > "$root/substitution-wrap-second.sh"
printf 'substitution-wrap-idempotent=%s\n' \
  "$(cmp -s "$root/substitution-wrap-formatted.sh" \
      "$root/substitution-wrap-second.sh" && printf yes)"

printf 'if\n' > "$root/invalid.sh"
before=$(cat "$root/invalid.sh")
"$BIN" --format --apply "$root/invalid.sh" >/dev/null 2>&1
printf 'parse-status=%s unchanged=%s\n' "$?" \
  "$([ "$(cat "$root/invalid.sh")" = "$before" ] && printf yes)"

printf '#!/bin/sh\n[ "\0441" == y ]\nif\n' > "$root/invalid-lint.sh"
cp "$root/invalid-lint.sh" "$root/invalid-lint.expected"
"$BIN" --lint --apply --no-traces "$root/invalid-lint.sh" \
  >/dev/null 2>&1
printf 'lint-parse-status=%s unchanged=%s\n' "$?" \
  "$(cmp -s "$root/invalid-lint.sh" "$root/invalid-lint.expected" && \
      printf yes)"
"$BIN" --lint --format --apply --no-traces "$root/invalid-lint.sh" \
  >/dev/null 2>&1
printf 'combined-parse-status=%s unchanged=%s\n' "$?" \
  "$(cmp -s "$root/invalid-lint.sh" "$root/invalid-lint.expected" && \
      printf yes)"

"$BIN" --apply "$root/first.sh" >/dev/null 2>&1
printf 'apply-alone-status=%s\n' "$?"
"$BIN" --format "$root/first.sh" "$root/second.sh" >/dev/null 2>&1
printf 'multi-stdout-status=%s\n' "$?"
printf '#!/bin/sh\nif test -n "\0441"; then [ "\0441" == y ]; fi\n' |
  "$BIN" --lint --format --no-traces > "$root/lint-format.out"
printf 'lint-format-stdout-status=%s\n' "$?"
cat "$root/lint-format.out"

printf '#!/bin/sh\n[ "\0441" == y ]\n' > "$root/conflict.sh"
"$BIN" --lint --apply -s "$root/conflict.sh" >/dev/null 2>&1
stdin_conflict_status=$?
"$BIN" --lint --apply -i "$root/conflict.sh" >/dev/null 2>&1
interactive_conflict_status=$?
"$BIN" --lint --apply -c : "$root/conflict.sh" >/dev/null 2>&1
command_conflict_status=$?
printf 'apply-conflicts=%s,%s,%s unchanged=%s\n' \
  "$stdin_conflict_status" "$interactive_conflict_status" \
  "$command_conflict_status" \
  "$([ "$(tail -1 "$root/conflict.sh")" = '[ "$1" == y ]' ] && printf yes)"
"$BIN" --format --apply "$root" >/dev/null 2>&1
printf 'nonregular-status=%s\n' "$?"
KOSH_FLAGS='--format --apply' "$BIN" -c 'printf environment-options-filtered\\n'

cat > "$root/adjacent-parens.sh" <<'EOF'
( (a))
( (test a = b) || (test a != b))
((a))
(( a = 1 ))
EOF
adjacent=$("$BIN" --format "$root/adjacent-parens.sh")
printf 'adjacent-status=%s\n' "$?"
printf '%s\n' "$adjacent"
adjacent_second=$(printf '%s\n' "$adjacent" | "$BIN" --format)
printf 'adjacent-idempotent=%s\n' \
  "$([ "$adjacent" = "$adjacent_second" ] && printf yes)"
