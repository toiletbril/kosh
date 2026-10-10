#!/bin/bash
# The tab-stripping operator takes its delimiter after a blank as well as
# attached, a delimiter starting with a dash after << is an ordinary
# delimiter, <<- with no delimiter is a syntax error, and a lone backslash
# at the end of the input is literal.
work=$(mktemp -d)
cd "$work" || exit 1
cat <<- EOF
	spaced
	EOF
cat <<-EOF
	attached
	EOF
cat <<-'Q'
	$quoted
	Q
cat << -
dash-delimited
-
cat <<- -
	stripped-dash
	-
printf 'cat <<-\nx\n' > missing-delimiter.sh
"$BASH" -n missing-delimiter.sh 2> /dev/null
echo "missing-delimiter=$?"
eval 'echo trailing\'
printf 'echo file-end \\' > trailing-backslash.sh
. ./trailing-backslash.sh
cd / && rm -rf -- "$work"
