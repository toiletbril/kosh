#!/bin/bash
# The dotglob and nullglob shopts steer the glob, checked byte-for-byte against
# bash. With dotglob off a leading-dot-less pattern skips the hidden entry, with
# it on the hidden entry joins the match. With nullglob off a pattern with no
# match stays literal, with it on the pattern expands to nothing.
initial_directory=$PWD
d=$(mktemp -d)
touch "$d/.hidden" "$d/visible"
cd "$d" || exit 1
echo "plain: "*
shopt -s dotglob
echo "dotglob: "*
shopt -u dotglob
echo "nomatch_literal: "nope*
shopt -s nullglob
echo "nomatch_null: "nope*
echo "dotglob_null: "*
shopt -u nullglob
cd "$initial_directory" || exit 1
rm -rf "$d"


# Bash extended globs, checked byte-for-byte against bash. The groups ?(..),
# *(..), +(..), @(..), and !(..) match in the [[ ]] pattern, in a case label, in
# a parameter expansion, and against filenames. bash needs extglob set before it
# parses, so the option is set first.
shopt -s extglob
[[ abc == @(abc|xyz) ]] && echo at-one
[[ abcabc == +(abc) ]] && echo plus
[[ color == c?(olo)r ]] && echo opt
[[ aXXc == a*(X)c ]] && echo star
[[ foo == !(bar) ]] && echo neg
[[ bar == @(x|y) ]] || echo no-match
v=foobar
echo "${v##+(fo)}"
f=image.jpg
echo "${f%%@(.jpg|.png)}"
case hello in @(hi|hello)) echo case-yes;; esac
case zzz in !(a|b)) echo case-neg;; esac
dir=/tmp/kosh_extglob_test_$$
rm -rf "$dir"; mkdir -p "$dir"
( cd "$dir" && touch a.txt b.txt c.log foo bar )
( cd "$dir" && echo @(a|b).txt )
( cd "$dir" && echo @(foo|bar) )
( cd "$dir" && echo !(*.txt) )
rm -rf "$dir"

# The [:name:] classes inside a bracket, across the replace, trim, [[ match,
# and case contexts, the ${cur//[[:space:]]/} construct bash-completion
# filters tokens with. A class never opens a range and an unknown class
# matches nothing.
x=" a b "
echo "strip=[${x//[[:space:]]/}]"
echo "mark=[${x//[[:space:]]/_}]"
x="a1b2c"
echo "digits=[${x//[[:digit:]]/D}]"
x="Hello World"
echo "upper=[${x//[[:upper:]]/U}]"
x="ab"
echo "trim=[${x#[[:alpha:]]}]"
[[ " " == [[:space:]] ]] && echo "cond=space"
[[ "x" == [[:alpha:]] ]] && echo "cond=alpha"
[[ "x" == [[:digit:]] ]] || echo "cond=notdigit"
case g in
  [[:digit:]]) echo "case=digit" ;;
  [[:alpha:]]) echo "case=alpha" ;;
esac
x="a-b"
echo "range_member=[${x//[a-z]/_}]"
x="abc"
echo "unknown=[${x//[[:bogus:]]/_}]"
x="]x"
echo "bracket_member=[${x//[]]/_}]"
x="tab	end"
echo "blank=[${x//[[:blank:]]/_}]"

# globskipdots, on by default since bash 5.3, keeps . and .. out of a dot glob.
# The temp directory holds one hidden and one plain entry, so the .* glob lists
# the hidden entry alone and the * glob lists the plain one. With globskipdots
# off the .* glob lists . and .. alongside the hidden entry.
d=$(mktemp -d)
touch "$d/.hidden" "$d/visible"
cd "$d" || exit 1
echo .*
echo *
shopt -u globskipdots
echo .*
cd "$initial_directory" || exit 1
rm -rf "$d"

# Bash globstar **, checked byte-for-byte against bash. The ** matches across
# directory levels when shopt globstar is on, as a trailing component it lists
# every file and directory recursively, and in a path position it stands in for
# zero or more levels. Without globstar ** behaves like *.
dir=/tmp/kosh_globstar_test_$$
rm -rf "$dir"
mkdir -p "$dir/a/b/c" "$dir/a/d"
touch "$dir/f0" "$dir/a/f1" "$dir/a/b/f2" "$dir/a/b/c/f3" "$dir/a/d/f4"
cd "$dir"
shopt -s globstar
echo "--- trailing ---"
for x in **; do echo "$x"; done
echo "--- dirs ---"
for x in **/; do echo "$x"; done
echo "--- middle ---"
for x in a/**/f*; do echo "$x"; done
echo "--- leading ---"
for x in **/f*; do echo "$x"; done
echo "--- base trailing ---"
for x in a/**; do echo "$x"; done
echo "--- off ---"
shopt -u globstar
for x in **; do echo "$x"; done
cd "$initial_directory"
rm -rf "$dir"
