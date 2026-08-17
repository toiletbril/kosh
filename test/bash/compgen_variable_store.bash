#!/bin/bash
compgen -V stored -W "alpha beta apex" -- a
echo "store=$?"
declare -p stored

stored=(old)
compgen -V stored -W "alpha beta" -- z
echo "no-match=$?"
declare -p stored

compgen -V fresh
echo "no-generator=$?"
declare -p fresh

scalar=one
compgen -V scalar -W "alpha" -- a
echo "promoted=$?"
declare -p scalar

compgen -V dupes -W "a a b" -- a
declare -p dupes

compgen -V affixed -P pre -S suf -W "aa ab" -- a
declare -p affixed
compgen -P pre -S suf -W "aa ab" -- a
compgen -P pre -X "pre*" -W "aa ab" -- a
echo "raw-filter=$?"
compgen -P pre -X "a*" -W "aa ab" -- a
echo "prefixed-filter=$?"

compgen -V "bad[0]" -W a -- a 2>/dev/null
echo "invalid-name=$?"

declare -A assoc
compgen -V assoc -W a -- a 2>/dev/null
echo "not-indexed=$?"

readonly frozen=1
compgen -V frozen -W a -- a 2>/dev/null
echo "readonly=$?"

compgen -V first -V last -W a -- a
declare -p last
declare -p first 2>/dev/null
echo "unset-first=$?"

wrapper()
{
  local inner=(caller)
  compgen -V inner -W "aa ab" -- a
  declare -p inner
}
wrapper
declare -p inner 2>/dev/null
echo "local-target=$?"
