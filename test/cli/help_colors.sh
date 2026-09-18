unset KOSH_FLAGS
. ./capture-terminal-command.sh

BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")

check_contains()
{
  output=$1
  expected=$2
  name=$3
  case "$output" in
    *"$expected"*) echo "$name=passed" ;;
    *) echo "$name=failed" ;;
  esac
}

check_excludes()
{
  output=$1
  rejected=$2
  name=$3
  case "$output" in
    *"$rejected"*) echo "$name=failed" ;;
    *) echo "$name=passed" ;;
  esac
}

escape=$(printf '\033')
main_help=$(NO_COLOR= capture_terminal_command "exec \"$BIN\" --help") || exit 1
builtin_help=$(NO_COLOR= capture_terminal_command \
  "exec \"$BIN\" -c 'help set'") || exit 1
koshkit_help=$(NO_COLOR= capture_terminal_command \
  "exec \"$BIN\" -c 'koshkit --help'") || exit 1

check_contains "$main_help" \
  "${escape}[1;34mSYNOPSIS${escape}[0m" main-heading
check_contains "$main_help" "${escape}[32m  $BIN" main-synopsis
check_contains "$main_help" \
  "  -M, --mood${escape}[2m=<...>${escape}[0m" main-flag
check_contains "$builtin_help" \
  "  -M, --mood${escape}[2m=<...>${escape}[0m" builtin-flag
check_contains "$koshkit_help" \
  "${escape}[32m  koshkit --color" koshkit-flag
check_excludes "$main_help$builtin_help$koshkit_help" \
  "${escape}[1;37m" bold-white
