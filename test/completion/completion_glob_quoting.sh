# Inline glob completion (echo *<TAB>) shell-quotes each expanded filename, so a
# name with a space, parens, a glob character, a backtick, a dollar, a double
# quote, a bang, or a single quote inserts as valid shell input. The preferred
# order is single quotes, then double quotes when the name holds a single quote.
# A backtick, dollar, and double quote are escaped inside the double-quote form,
# while a bang stays literal since kosh has no history expansion.
d=$(mktemp -d)
cd "$d" || exit 1
# Names without a single quote take the single-quote form.
: > 'a b.txt'
: > 'foo(1).sh'
: > 'star*.txt'
: > 'back`tick.txt'
: > 'dollar$x.txt'
: > 'double".txt'
: > 'bang!.txt'
: > normal.txt
# Names with a single quote take the double-quote form.
: > "sq'back\`.txt"
: > "sq'dollar\$.txt"
: > "sq'double\".txt"
: > "sq'bang!.txt"
echo "== inline glob quoting:"
"$BIN" --debug-complete-at 'echo *' </dev/null 2>/dev/null
cd / || exit 1
rm -rf "$d"
