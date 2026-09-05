#!/bin/bash

trap 'echo ERR' ERR

echo guarded-loop
for item in one two; do
  false && echo unreachable
done
echo "status=$?"

echo plain-loop
for item in one two; do
  false
done
echo "status=$?"

echo brace
{ false; }
echo "status=$?"

echo function
fail_function() {
  false
}
fail_function
echo "status=$?"

echo subshell
(false)
echo "status=$?"

echo conditional
[[ no = yes ]]
echo "status=$?"

echo arithmetic
((0))
echo "status=$?"

echo errtrace-function
set -E
fail_function
echo "status=$?"

echo errtrace-subshell
(false)
echo "status=$?"

echo return-plain
trap 'echo RETURN' RETURN
return_function() {
  true
}
return_function
echo return-functrace
set -T
return_function
set +T
trap - RETURN
