#!/bin/bash
# Bash shopt builtin, checked byte-for-byte against bash. Sets, unsets, and
# queries the shell options and the -q exit status. The display line's padding
# width differs between bash builds, so the listing runs are squeezed through
# tr and compare on content rather than the column.
shopt -s extglob
shopt extglob | tr -s ' \t' ' '
shopt -u extglob
shopt extglob | tr -s ' \t' ' '
shopt -s globstar nullglob dotglob
shopt globstar | tr -s ' \t' ' '
shopt nullglob | tr -s ' \t' ' '
shopt dotglob | tr -s ' \t' ' '
shopt -q extglob
echo "extglob query: $?"
shopt -s extglob
shopt -q extglob
echo "after set: $?"
shopt -u extglob
shopt -q extglob
echo "after unset: $?"
shopt -u xpg_echo
case :$BASHOPTS: in
  *:xpg_echo:*) echo "bashopts-before=on" ;;
  *) echo "bashopts-before=off" ;;
esac
shopt -s xpg_echo
case :$BASHOPTS: in
  *:xpg_echo:*) echo "bashopts-after=on" ;;
  *) echo "bashopts-after=off" ;;
esac
(BASHOPTS=changed)
echo "bashopts-assign=$?"
unset BASHOPTS
echo "bashopts-unset=$?"
case :$BASHOPTS: in
  *:xpg_echo:*) echo "bashopts-preserved=on" ;;
  *) echo "bashopts-preserved=off" ;;
esac
shopt -s nocaseglob nocasematch
shopt nocaseglob | tr -s ' \t' ' '
shopt nocasematch | tr -s ' \t' ' '


shopt -qo posix; echo "posix=$?"
set -e; shopt -qo errexit; echo "e=$?"
set +e; shopt -qo errexit; echo "e2=$?"
shopt -qo nounset; echo "u=$?"
set -u; shopt -qo nounset; echo "u2=$?"
shopt -qs progcomp; echo "shopt-still-works=$?"

set +o interactive-comments
shopt -q interactive_comments; echo "interactive-comments-off=$?"
set -o interactive-comments
shopt -q interactive_comments; echo "interactive-comments-on=$?"
case :$SHELLOPTS: in
  *:interactive-comments:*) echo interactive-comments-listed ;;
  *) echo interactive-comments-missing ;;
esac

alias shopt_alias='echo alias-expanded'
shopt -u expand_aliases
shopt_alias 2>/dev/null; echo "alias-off=$?"
shopt -s expand_aliases
shopt_alias

value=aaab
shopt -s extglob
echo "extglob-on=${value##+(a)}"
shopt -u extglob
echo "extglob-off=${value##+(a)}"
