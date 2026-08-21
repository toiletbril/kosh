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

echo '== compacted divergent branches preserve uncertainty:'
"$BIN" -n -WWW -c 'seed_001=1 seed_002=1 seed_003=1 seed_004=1 seed_005=1 seed_006=1 seed_007=1 seed_008=1
seed_009=1 seed_010=1 seed_011=1 seed_012=1 seed_013=1 seed_014=1 seed_015=1 seed_016=1
seed_017=1 seed_018=1 seed_019=1 seed_020=1 seed_021=1 seed_022=1 seed_023=1 seed_024=1
seed_025=1 seed_026=1 seed_027=1 seed_028=1 seed_029=1 seed_030=1 seed_031=1 seed_032=1
seed_033=1 seed_034=1 seed_035=1 seed_036=1 seed_037=1 seed_038=1 seed_039=1 seed_040=1
seed_041=1 seed_042=1 seed_043=1 seed_044=1 seed_045=1 seed_046=1 seed_047=1 seed_048=1
seed_049=1 seed_050=1 seed_051=1 seed_052=1 seed_053=1 seed_054=1 seed_055=1 seed_056=1
seed_057=1 seed_058=1 seed_059=1 seed_060=1 seed_061=1 seed_062=1 seed_063=1 seed_064=1
seed_065=1 seed_066=1 seed_067=1 seed_068=1 seed_069=1 seed_070=1 seed_071=1 seed_072=1
seed_073=1 seed_074=1 seed_075=1 seed_076=1 seed_077=1 seed_078=1 seed_079=1 seed_080=1
seed_081=1 seed_082=1 seed_083=1 seed_084=1 seed_085=1 seed_086=1 seed_087=1 seed_088=1
seed_089=1 seed_090=1 seed_091=1 seed_092=1 seed_093=1 seed_094=1 seed_095=1 seed_096=1
seed_097=1 seed_098=1 seed_099=1 seed_100=1 seed_101=1 seed_102=1 seed_103=1 seed_104=1
seed_105=1 seed_106=1 seed_107=1 seed_108=1 seed_109=1 seed_110=1 seed_111=1 seed_112=1
seed_113=1 seed_114=1 seed_115=1 seed_116=1 seed_117=1 seed_118=1 seed_119=1 seed_120=1
seed_121=1 seed_122=1 seed_123=1 seed_124=1 seed_125=1 seed_126=1 seed_127=1 seed_128=1
removed_marker=1
if test -n "$1"; then
  unset removed_marker
  branch_special=1
  filler_001=1 filler_002=1 filler_003=1 filler_004=1 filler_005=1 filler_006=1 filler_007=1 filler_008=1
  filler_009=1 filler_010=1 filler_011=1 filler_012=1 filler_013=1 filler_014=1 filler_015=1 filler_016=1
  filler_017=1 filler_018=1 filler_019=1 filler_020=1 filler_021=1 filler_022=1 filler_023=1 filler_024=1
  filler_025=1 filler_026=1 filler_027=1 filler_028=1 filler_029=1 filler_030=1 filler_031=1 filler_032=1
  filler_033=1 filler_034=1 filler_035=1 filler_036=1 filler_037=1 filler_038=1 filler_039=1 filler_040=1
  filler_041=1 filler_042=1 filler_043=1 filler_044=1 filler_045=1 filler_046=1 filler_047=1 filler_048=1
  filler_049=1 filler_050=1 filler_051=1 filler_052=1 filler_053=1 filler_054=1 filler_055=1 filler_056=1
  filler_057=1 filler_058=1 filler_059=1 filler_060=1 filler_061=1 filler_062=1 filler_063=1 filler_064=1
  filler_065=1 filler_066=1 filler_067=1 filler_068=1 filler_069=1 filler_070=1 filler_071=1 filler_072=1
  filler_073=1 filler_074=1 filler_075=1 filler_076=1 filler_077=1 filler_078=1 filler_079=1 filler_080=1
  filler_081=1 filler_082=1 filler_083=1 filler_084=1 filler_085=1 filler_086=1 filler_087=1 filler_088=1
  filler_089=1 filler_090=1 filler_091=1 filler_092=1 filler_093=1 filler_094=1 filler_095=1 filler_096=1
  filler_097=1 filler_098=1 filler_099=1 filler_100=1 filler_101=1 filler_102=1 filler_103=1 filler_104=1
  filler_105=1 filler_106=1 filler_107=1 filler_108=1 filler_109=1 filler_110=1 filler_111=1 filler_112=1
  filler_113=1 filler_114=1 filler_115=1 filler_116=1 filler_117=1 filler_118=1 filler_119=1 filler_120=1
  filler_121=1 filler_122=1 filler_123=1 filler_124=1 filler_125=1 filler_126=1 filler_127=1 filler_128=1
  if test -n "$2"; then nested_value=one; else nested_value=two; fi
fi
echo "$removed_marker $seed_128 $branch_special $nested_value"' 2>&1

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
