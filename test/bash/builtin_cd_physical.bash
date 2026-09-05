#!/bin/bash
# cd -P resolves the symlinks to the physical directory, while -L and the default
# keep the logical path the operand spelled.
d=$(mktemp -d)
mkdir -p "$d/real"
ln -sfn "$d/real" "$d/link"
echo "physical=$(cd -P "$d/link" && pwd | grep -o '/real$')"
echo "logical=$(cd -L "$d/link" && pwd | grep -o '/link$')"
echo "default=$(cd "$d/link" && pwd | grep -o '/link$')"
start=$PWD
mkdir -p "$d/cdable" "$d/cdpath/literalname"
cdable_target="$d/cdable"
literalname="$d/cdable"
shopt -s cdable_vars
cd cdable_target >/dev/null
[ "$PWD" = "$d/cdable" ] && echo cdable-variable=yes
cd "$d"
empty_target=
cd empty_target >/dev/null
[ "$PWD" = "$d" ] && echo cdable-empty=yes
CDPATH="$d/cdpath"
cd literalname >/dev/null
[ "$PWD" = "$d/cdpath/literalname" ] && echo cdable-cdpath-first=yes
unset CDPATH
shopt -u cdable_vars
cd "$start"
rm -rf "$d"


# cd into a symlinked directory and back out with cd .., the bash -L default. PWD
# keeps the symlink path while pwd -P shows the resolved target, and cd .. returns
# to the directory holding the link rather than the physical parent. Checked byte
# for byte against bash.
base=$(mktemp -d)
mkdir -p "$base/real/sub"
ln -s "$base/real/sub" "$base/link"
cd "$base"
cd link
echo "in_link=$([ "$PWD" = "$base/link" ] && echo yes || echo no)"
echo "physical_differs=$([ "$(pwd -P)" != "$PWD" ] && echo yes || echo no)"
cd ..
echo "after_dotdot=$([ "$PWD" = "$base" ] && echo yes || echo no)"

mkdir -p "$base/current/child" "$base/forged/child"
cd "$base/current"
PWD="$base/forged"
cd child
echo "forged_pwd_ignored=$([ "$(pwd -P)" = "$base/current/child" ] && echo yes || echo no)"
rm -rf "$base"

# A bare tilde before a colon expands the same as bash. In an assignment value
# each colon-delimited segment expands its leading tilde, and a regular word
# expands only its own leading tilde while a tilde after a colon stays literal.
# A quoted tilde is left alone.
p=~:~; echo "$p"
x=~/a:~/b; echo "$x"
echo ~:x
echo ~/a:~/b
echo ~:~
echo a:~
echo "~:x"
