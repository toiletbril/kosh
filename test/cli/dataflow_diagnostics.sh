#!/bin/sh

unset KOSH_FLAGS

echo '== a name that is never assigned:'
"$BIN" -n -WWW -c 'echo "$undefined_name"' 2>&1

echo '== a name that resembles an assigned name:'
"$BIN" -n -WWW -c 'my_value=1
echo "$myvalue"' 2>&1

echo '== case and underscore folding both count as a resemblance:'
"$BIN" -n -WWW -c 'output_file=out
echo "$OUTPUTFILE"' 2>&1

echo '== an assignment before the read stays quiet:'
"$BIN" -n -WWW -c 'name=1
echo "$name"' 2>&1
echo "rc=$?"

echo '== a prefix assignment counts as an assignment:'
"$BIN" -n -WWW -c 'w=1 b=2
echo "$w$b"' 2>&1
echo "rc=$?"

echo '== the shell maintains these names itself:'
"$BIN" -n -WWW -c 'echo "$OPTIND $OPTARG $REPLY $RANDOM $LINENO $SECONDS $PPID"' 2>&1
echo "rc=$?"

echo '== a KOSH_ name is maintained by the shell:'
"$BIN" -n -WWW -c 'echo "$KOSH_GIT_BRANCH $KOSH_ANSI_RED"' 2>&1
echo "rc=$?"

echo '== an arithmetic assignment assigns its target:'
"$BIN" -n -WWW -c 'echo $((y = 7)); echo "$y"' 2>&1
echo "rc=$?"

echo '== a compound arithmetic assignment assigns its target:'
"$BIN" -n -WWW -c 'echo $((b += 2)); echo $((c <<= 1)); echo "$b$c"' 2>&1
echo "rc=$?"

echo '== an arithmetic comparison assigns nothing:'
"$BIN" -n -WWW -c 'echo $((q <= 1)); echo "$q"' 2>&1

echo '== a C-style for init clause assigns its target:'
"$BIN" -n -WWW -c 'for ((x = 5; 0; x++)); do echo never; done
echo "$x"' 2>&1
echo "rc=$?"

echo '== the default mood rejects an unassigned read:'
"$BIN" -n -c 'echo "$rejected_name"' 2>&1
echo "rc=$?"

echo '== a disable directive silences the sweep:'
"$BIN" -n -WWW -c '# shellcheck disable=SC2154
echo "$suppressed_name"' 2>&1
echo "rc=$?"
