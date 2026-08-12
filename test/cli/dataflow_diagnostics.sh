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

echo '== a transposed pair still names the assigned variable:'
"$BIN" -n -WWW -c 'output_length=1
echo "$outupt_length"' 2>&1

echo '== a dropped byte still names the assigned variable:'
"$BIN" -n -WWW -c 'destination_path=1
echo "$destinaton_path"' 2>&1

echo '== two edits on a long name still resolve:'
"$BIN" -n -WWW -c 'configuration_root=1
echo "$configuratoin_rot"' 2>&1

echo '== a name too far from every assigned name stays unresolved:'
"$BIN" -n -WWW -c 'destination_path=1
echo "$temporary_buffer"' 2>&1

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

echo '== a read before a later assignment names that assignment:'
"$BIN" -n -WWW -c 'echo "$greeting_text"
greeting_text=hello' 2>&1

echo '== a disable directive silences the sweep:'
"$BIN" -n -WWW -c '# shellcheck disable=SC2154
echo "$suppressed_name"' 2>&1
echo "rc=$?"

echo '== an assign form parameter expansion assigns its target:'
"$BIN" -n -WWW -c 'echo "${assigned_target:=written}"
echo "$assigned_target"' 2>&1
echo "rc=$?"

echo '== a default form parameter expansion assigns nothing:'
"$BIN" -n -WWW -c 'echo "${default_target:-written}"
echo "$default_target"' 2>&1

echo '== a prefix on an ordinary command does not outlive it:'
"$BIN" -n -WWW -c 'temporary_name=1 true
echo "$temporary_name"' 2>&1

echo '== a prefix on a special builtin outlives it:'
"$BIN" -n -WWW -c 'persistent_name=1 export other_name=2
echo "$persistent_name"' 2>&1
echo "rc=$?"

echo '== a prefix repeating its own value is not a self assignment:'
"$BIN" -n -WWW -c 'kept_name=1
kept_name="$kept_name" env true' 2>&1
echo "rc=$?"

echo '== a command valued prefix keeps its command name:'
"$BIN" -n -WWW -c 'PAGER=cat true' 2>&1
echo "rc=$?"
