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

echo '== an if-only assignment remains conditional:'
"$BIN" -n -WWW -c 'if test -e /; then if_value=1; fi
echo "$if_value"' 2>&1

echo '== an assignment on every if exit stays quiet:'
"$BIN" -n -WWW -c 'if test -e /; then branch_value=1; else branch_value=2; fi
echo "$branch_value"' 2>&1
echo "rc=$?"

echo '== loop-only assignments remain conditional:'
"$BIN" -n -WWW -c 'for entry in "$@"; do for_value=1; done
while test -e /; do while_value=1; break; done
echo "$for_value $while_value"' 2>&1

echo '== short-circuit assignments remain conditional:'
"$BIN" -n -WWW -c 'false && and_value=1
true || or_value=1
echo "$and_value $or_value"' 2>&1

echo '== a prefix assignment counts as an assignment:'
"$BIN" -n -WWW -c 'w=1 b=2
echo "$w$b"' 2>&1
echo "rc=$?"

echo '== the shell maintains these names itself:'
"$BIN" -n -WWW -c 'echo "$OPTIND $OPTARG $REPLY $RANDOM $LINENO $SECONDS $PPID"' 2>&1
echo "rc=$?"

echo '== an exact environment value stays quiet:'
DATAFLOW_EXPORTED_VALUE=one
export DATAFLOW_EXPORTED_VALUE
"$BIN" -n -WWW -c 'echo "$DATAFLOW_EXPORTED_VALUE"' 2>&1
echo "rc=$?"
unset DATAFLOW_EXPORTED_VALUE

echo '== an environment typo remains unresolved:'
DATAFLOW_EXPORTED_VALUE=one
export DATAFLOW_EXPORTED_VALUE
"$BIN" -n -WWW -c 'echo "$DATAFLOW_EXPORTED_VLAUE"' 2>&1
unset DATAFLOW_EXPORTED_VALUE

echo '== a KOSH_ name is maintained by the shell:'
"$BIN" -n -WWW -c 'echo "$KOSH $KOSH_FLAGS $KOSH_GIT_BRANCH $KOSH_ANSI_RED"' 2>&1
echo "rc=$?"

echo '== misspelled KOSH and ANSI names are unresolved:'
"$BIN" -n -WWW -c 'echo "$KOSH_GIT_BRNACH $KOSH_ANSI_REDD $KOSH_VERSOIN $OPTNID"' 2>&1

echo '== sh mood does not suggest a Bash-only variable:'
"$BIN" --mood sh -n -WWW -c 'echo "$BASH_VERSOIN"' 2>&1

echo '== unavailable catalog names are not suggested:'
"$BIN" --mood bash -n -WWW -c 'echo "$BASHOPTZ"' 2>&1
"$BIN" --mood sh -n -WWW -c 'echo "$GLOBIGNROE"' 2>&1

echo '== every assignment binder describes its target:'
"$BIN" --mood bash -n -WWW -c 'for loop_entry in "$@"; do :; done
select menu_choice in one; do break; done
(( arithmetic_value = 1 ))
read -r read_field
mapfile -t mapped_rows
getopts "a" option_letter
printf -v formatted_text "%s" value
declare declared_name
export exported_name
readonly readonly_name
echo "$loop_entyr"
echo "$menu_choiec"
echo "$arithmetic_vlaue"
echo "$read_feild"
echo "$mapped_riws"
echo "$option_leter"
echo "$formated_text"
echo "$declraed_name"
echo "$exported_nmae"
echo "$readonly_nmae"' 2>&1

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

echo '== a read before a runtime definer is still reported:'
"$BIN" -n -WWW -c 'echo "$before_eval"
eval "before_eval=1"' 2>&1

echo '== a read after a runtime definer stays quiet:'
"$BIN" -n -WWW -c 'eval "after_eval=1"
echo "$after_eval"' 2>&1
echo "rc=$?"

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
