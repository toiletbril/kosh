#include "../Builtin.hpp"
#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[option ...] target");
HELP_DESCRIPTION_DECL(
    "The assimilate builtin installs this shell on a remote SSH target.");

FLAG(TRACE, Bool, 'x', "trace", "Trace the remote installation commands.");
FLAG(SSH_COMMAND, String, '\0', "ssh-command",
     "Use this space-separated command instead of ssh.");
FLAG(SCP_COMMAND, String, '\0', "scp-command",
     "Use this space-separated command instead of scp.");
FLAG(LINK_MOOD, ManyStrings, '\0', "link-mood",
     "Link this binary as bash, dash, sh, or kosh. Commas are accepted.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_BUILTIN_FLAGS(Assimilate);

namespace koshka {

namespace {

enum class trace_output : u8
{
  Disabled,
  Enabled,
};

constexpr char REMOTE_TRANSACTION_TEXT[] = R"SH(set -eu
[ "${5-0}" -eq 0 ] || set -x
umask 077
upload=$1
link_moods=${4-}
candidate=
target=
lock=
backup=
lock_owned=0
lock_published=0
had_target=0
rollback_active=0
committed=0
remove_recorded_links() {
  links_file=$1
  [ ! -r "$links_file" ] || while IFS= read -r link_name; do
    case $link_name in bash|dash|sh) ;; *) return 1 ;; esac
    link_path=${links_file%/*}/../$link_name
    [ ! -L "$link_path" ] || koshkit rm -f "$link_path" || return 1
  done < "$links_file"
}
cleanup() {
  transaction_status=$?
  [ "$#" -eq 0 ] || transaction_status=$1
  if [ "$rollback_active" -eq 1 ] && [ "$committed" -eq 0 ] &&
     [ "$transaction_status" -eq 0 ]; then
    transaction_status=1
  fi
  cleanup_failed=0
  trap - 0 1 2 15
  if [ "$lock_owned" -eq 1 ]; then
    [ -n "$candidate" ] || [ ! -r "$lock/candidate" ] || candidate=$(koshkit cat "$lock/candidate")
    [ -n "$upload" ] || [ ! -r "$lock/upload" ] || upload=$(koshkit cat "$lock/upload")
    if [ "$rollback_active" -eq 1 ] && [ "$committed" -eq 0 ]; then
      remove_recorded_links "$lock/links" || cleanup_failed=1
      if [ "$had_target" -eq 1 ] && { [ -e "$backup" ] || [ -L "$backup" ]; }; then
        koshkit mv -f "$backup" "$target" || cleanup_failed=1
      elif [ "$had_target" -eq 1 ]; then
        [ -e "$target" ] || [ -L "$target" ] || cleanup_failed=1
      else
        koshkit rm -f "$target" || cleanup_failed=1
      fi
    fi
    [ -z "$candidate" ] || koshkit rm -f "$candidate" || cleanup_failed=1
    [ -z "$upload" ] || koshkit rm -f "$upload" || cleanup_failed=1
    if [ "$committed" -eq 1 ] || [ "$rollback_active" -eq 0 ]; then
      [ -z "$backup" ] || koshkit rm -f "$backup" || cleanup_failed=1
    fi
    if [ "$cleanup_failed" -eq 0 ]; then
      koshkit rm -f "$lock/upload" "$lock/candidate" \
        "$lock/had-target" "$lock/rollback" "$lock/committed" \
        "$lock/links" || cleanup_failed=1
      [ "$lock_published" -eq 0 ] || koshkit rm -f "$lock_path" || cleanup_failed=1
      if [ "$cleanup_failed" -eq 0 ]; then
        koshkit rm -f "$lock/owner" || cleanup_failed=1
        [ "$cleanup_failed" -ne 0 ] || koshkit rmdir "$lock" || cleanup_failed=1
      fi
    fi
  else
    [ -z "$candidate" ] || koshkit rm -f "$candidate" || cleanup_failed=1
    [ -z "$upload" ] || koshkit rm -f "$upload" || cleanup_failed=1
  fi
  [ "$cleanup_failed" -eq 0 ] || exit 1
  exit "$transaction_status"
}
load_stale_lock_record() {
  stale_owner=${lock_record%%:*}
  stale_lock_name=${lock_record#*:}
  [ "$stale_lock_name" != "$lock_record" ] || return 1
  case $stale_owner in ''|*[!0-9]*) return 1 ;; esac
  case $stale_lock_name in .kosh-assimilate-*.lock) ;; *) return 1 ;; esac
  stale_transaction_id=${stale_lock_name#.kosh-assimilate-}
  stale_transaction_id=${stale_transaction_id%.lock}
  stale_process_id=${stale_transaction_id%%-*}
  stale_timestamp=${stale_transaction_id#*-}
  [ "$stale_timestamp" != "$stale_transaction_id" ] || return 1
  case $stale_process_id in ''|*[!0-9]*) return 1 ;; esac
  case $stale_timestamp in ''|*[!0-9]*) return 1 ;; esac
  [ "$stale_transaction_id" = "$stale_process_id-$stale_timestamp" ] || return 1
  stale_lock=$install_dir/$stale_lock_name
  [ -d "$stale_lock" ] && [ ! -L "$stale_lock" ] || return 1
  stale_upload=.kosh-assimilate-$stale_transaction_id.upload
  stale_candidate=$install_dir/.kosh-assimilate-$stale_transaction_id.candidate
  [ "${1-0}" -eq 0 ] || return 0
  [ ! -L "$stale_lock/upload" ] || return 1
  [ ! -L "$stale_lock/candidate" ] || return 1
  if [ -e "$stale_lock/upload" ]; then
    recorded_upload=$(koshkit cat "$stale_lock/upload") || return 1
    [ "$recorded_upload" = "$stale_upload" ] || return 1
  fi
  if [ -e "$stale_lock/candidate" ]; then
    recorded_candidate=$(koshkit cat "$stale_lock/candidate") || return 1
    [ "$recorded_candidate" = "$stale_candidate" ] || return 1
  fi
}
recover_stale_lock() {
  stale_backup=$stale_lock/backup
  if [ ! -e "$stale_lock/committed" ] && [ -e "$stale_lock/rollback" ]; then
    remove_recorded_links "$stale_lock/links" || return 1
    if [ -e "$stale_lock/had-target" ]; then
      if [ -e "$stale_backup" ] || [ -L "$stale_backup" ]; then
        koshkit mv -f "$stale_backup" "$target" || return 1
      else
        [ -e "$target" ] || [ -L "$target" ] || return 1
      fi
    else
      koshkit rm -f "$target" || return 1
    fi
  fi
  koshkit rm -f "$stale_candidate" "$stale_upload" "$stale_backup" || return 1
  koshkit rm -f "$stale_lock/upload" "$stale_lock/candidate" \
    "$stale_lock/had-target" "$stale_lock/rollback" \
    "$stale_lock/committed" "$stale_lock/links" || return 1
  if [ "${1-0}" -eq 1 ]; then
    current_lock_record=$(koshkit cat "$lock_path") || return 1
    [ "$current_lock_record" = "$lock_record" ] || return 1
    koshkit rm -f "$lock_path" || return 1
  fi
  koshkit rm -f "$stale_lock/owner" || return 1
  koshkit rmdir "$stale_lock" || return 1
}
recover_existing_lock() {
  lock_record=$(koshkit cat "$lock_path") || return 1
  load_stale_lock_record || return 1
  recover_stale_lock 1
}
recover_orphan_locks() {
  for orphan_lock in "$install_dir"/.kosh-assimilate-*.lock; do
    [ -d "$orphan_lock" ] || continue
    [ ! -L "$orphan_lock" ] || return 1
    if [ ! -e "$orphan_lock/rollback" ] && [ ! -e "$orphan_lock/committed" ]; then
      lock_record=0:${orphan_lock##*/}
      load_stale_lock_record 1 || return 1
    else
      lock_record=$(koshkit cat "$orphan_lock/owner") || return 1
      load_stale_lock_record || return 1
    fi
    [ "$stale_lock" = "$orphan_lock" ] || return 1
    recover_stale_lock 0 || return 1
  done
}
recover_all_stale_locks() {
  if [ -e "$lock_path" ] || [ -L "$lock_path" ]; then
    recover_existing_lock || return 1
  fi
  recover_orphan_locks
}
path_contains_directory() {
  wanted_directory=$1
  remaining_path=${PATH-}
  while :; do
    case $remaining_path in
      *:*) path_entry=${remaining_path%%:*}; remaining_path=${remaining_path#*:}; has_more=1 ;;
      *) path_entry=$remaining_path; has_more=0 ;;
    esac
    [ -n "$path_entry" ] || path_entry=.
    [ "$path_entry" != "$wanted_directory" ] || return 0
    [ "$has_more" -eq 1 ] || return 1
  done
}
directory_accepts_links() {
  remaining_moods=$link_moods
  while [ -n "$remaining_moods" ]; do
    case $remaining_moods in
      *,*) link_name=${remaining_moods%%,*}; remaining_moods=${remaining_moods#*,} ;;
      *) link_name=$remaining_moods; remaining_moods= ;;
    esac
    [ "$link_name" = kosh ] && continue
    link_path=$install_dir/$link_name
    if [ -e "$link_path" ] || [ -L "$link_path" ]; then
      [ -L "$link_path" ] && [ "$link_path" -ef "$target" ] || return 1
    fi
  done
}
try_install_directory() {
  proposed_directory=$1
  case $proposed_directory in *sbin*) return 1 ;; esac
  [ -d "$proposed_directory" ] && [ -w "$proposed_directory" ] &&
    [ -x "$proposed_directory" ] || return 1
  install_dir=$(cd "$proposed_directory" && pwd -P) || return 1
  target=$install_dir/kosh
  lock_path=$install_dir/.kosh-assimilate.lock
  recover_all_stale_locks || return 1
  directory_accepts_links || return 1
}
trap cleanup 0
trap 'exit 129' 1
trap 'exit 130' 2
trap 'exit 143' 15
install_dir=
if path_contains_directory /usr/local/bin; then
  try_install_directory /usr/local/bin || install_dir=
fi
if [ -z "$install_dir" ] && [ -n "${HOME-}" ] &&
   path_contains_directory "$HOME/.local/bin"; then
  try_install_directory "$HOME/.local/bin" || install_dir=
fi
remaining_path=${PATH-}
while [ -z "$install_dir" ]; do
  case $remaining_path in
    *:*) path_entry=${remaining_path%%:*}; remaining_path=${remaining_path#*:}; has_more=1 ;;
    *) path_entry=$remaining_path; has_more=0 ;;
  esac
  [ -n "$path_entry" ] || path_entry=.
  case $path_entry in
    /usr/local/bin) ;;
    "${HOME-}/.local/bin") ;;
    *) try_install_directory "$path_entry" || install_dir= ;;
  esac
  if [ -n "$install_dir" ]; then
    break
  fi
  [ "$has_more" -eq 1 ] || break
done
[ -n "$install_dir" ] || exit 1
candidate=$install_dir/.kosh-assimilate-$2.candidate
[ ! -d "$target" ] || exit 1
recover_all_stale_locks
trap '' 1 2 15
new_lock=$install_dir/.kosh-assimilate-$2.lock
koshkit mkdir "$new_lock"
lock=$new_lock
lock_owned=1
backup=$lock/backup
printf '%s:%s\n' "$$" "${lock##*/}" > "$lock/owner"
printf '%s\n' "$upload" > "$lock/upload"
printf '%s\n' "$candidate" > "$lock/candidate"
: > "$lock/links"
if ! koshkit ln -s "$lock/owner" "$lock_path"; then
  recover_existing_lock
  koshkit ln -s "$lock/owner" "$lock_path"
fi
lock_published=1
trap 'exit 129' 1
trap 'exit 130' 2
trap 'exit 143' 15
koshkit mv "$upload" "$candidate"
if ! candidate_identity=$("$candidate" -p --mood sh -c 'printf "%s\n" "$KOSH_IDENTITY"'); then
  cleanup 1
fi
[ -n "$candidate_identity" ] && [ "$candidate_identity" = "$3" ] || cleanup 1
if [ -e "$target" ] || [ -L "$target" ]; then
  had_target=1
  : > "$lock/had-target"
fi
rollback_active=1
: > "$lock/rollback"
[ "$had_target" -eq 0 ] || koshkit mv -f "$target" "$backup"
koshkit mv -f "$candidate" "$target"
candidate=
if ! target_identity=$("$target" -p --mood sh -c 'printf "%s\n" "$KOSH_IDENTITY"'); then
  cleanup 1
fi
[ -n "$target_identity" ] && [ "$target_identity" = "$candidate_identity" ] || cleanup 1
remaining_moods=$link_moods
while [ -n "$remaining_moods" ]; do
  case $remaining_moods in
    *,*) link_name=${remaining_moods%%,*}; remaining_moods=${remaining_moods#*,} ;;
    *) link_name=$remaining_moods; remaining_moods= ;;
  esac
  [ "$link_name" = kosh ] && continue
  link_path=$install_dir/$link_name
  if [ -e "$link_path" ] || [ -L "$link_path" ]; then
    [ -L "$link_path" ] && [ "$link_path" -ef "$target" ] || cleanup 1
  else
    koshkit ln -s kosh "$link_path" || cleanup 1
    printf '%s\n' "$link_name" >> "$lock/links"
  fi
done
trap '' 1 2 15
if ! : > "$lock/committed"; then
  cleanup 1
  exit 1
fi
committed=1
rollback_active=0
koshkit rm -f "$lock/rollback"
[ "$had_target" -eq 0 ] || koshkit rm -f "$backup"
koshkit rm -f "$lock/upload" "$lock/candidate" \
  "$lock/had-target" "$lock/committed" "$lock/links"
koshkit rm -f "$lock_path"
lock_published=0
koshkit rm -f "$lock/owner"
koshkit rmdir "$lock"
lock_owned=0
lock=
koshkit rm -f "$upload"
trap - 0 1 2 15
printf '%s\n' "$target"
)SH";
constexpr StringView REMOTE_TRANSACTION{REMOTE_TRANSACTION_TEXT,
                                        sizeof(REMOTE_TRANSACTION_TEXT) - 1};

constexpr char REMOTE_PREFLIGHT_TEXT[] = R"SH(set -eu
[ "$1" -eq 0 ] || set -x
expected_system=$2
expected_machine=$3
if ! remote_system=$(uname -s 2>/dev/null); then
  printf '%s\n' 'kosh: assimilate: Cannot determine the remote operating system.' >&2
  exit 125
fi
if ! remote_machine=$(uname -m 2>/dev/null); then
  printf '%s\n' 'kosh: assimilate: Cannot determine the remote architecture.' >&2
  exit 125
fi
case $expected_machine in
  arm64|aarch64) expected_machine=arm64 ;;
  x86_64|amd64) expected_machine=x86_64 ;;
esac
case $remote_machine in
  arm64|aarch64) remote_machine=arm64 ;;
  x86_64|amd64) remote_machine=x86_64 ;;
esac
system_matches=0
machine_matches=0
[ "$expected_system" = any ] || [ "$expected_system" = "$remote_system" ] || system_matches=1
[ "$expected_machine" = any ] || [ "$expected_machine" = "$remote_machine" ] || machine_matches=1
if [ "$system_matches" -ne 0 ] || [ "$machine_matches" -ne 0 ]; then
  printf 'kosh: assimilate: Cannot install a %s/%s binary on %s/%s.\n' \
    "$expected_system" "$expected_machine" "$remote_system" "$remote_machine" >&2
  exit 126
fi
)SH";
constexpr StringView REMOTE_PREFLIGHT{REMOTE_PREFLIGHT_TEXT,
                                      sizeof(REMOTE_PREFLIGHT_TEXT) - 1};

fn trace_stage(const ExecContext &ec, trace_output trace,
               StringView stage) throws -> void
{
  if (trace != trace_output::Enabled) return;

  let message = String{heap_allocator(), "+ assimilate: "};
  message += stage;
  message += '\n';
  ec.print_to_stderr(message.view());
}

fn run_program(ArrayList<String> arguments, const ExecContext &ec,
               trace_output trace, StringView stage) throws -> i32
{
  trace_stage(ec, trace, stage);
  let const result = os::run_measured(arguments, os::measured_output::Inherit);
  if (!result.has_value()) return 126;

  return static_cast<i32>(result->exit_status);
}

fn resolve_transfer_program(StringView name, ProgramResolver &resolver) throws
    -> Maybe<Path>
{
  let const matches = resolver.search(name, ProgramResolver::SearchMode::First,
                                      ProgramResolver::Requirement::Runnable,
                                      ProgramResolver::CachePolicy::Bypass);
  if (matches.is_empty()) return None;

  return matches[0].clone();
}

fn parse_transport_command(StringView spelling, ProgramResolver &resolver,
                           Allocator allocator) throws
    -> Maybe<ArrayList<String>>
{
  let arguments = ArrayList<String>{allocator};
  usize position = 0;
  while (position < spelling.count()) {
    while (position < spelling.count() && spelling[position] == ' ')
      position++;
    if (position == spelling.count()) break;

    let const start = position;
    while (position < spelling.count() && spelling[position] != ' ')
      position++;
    arguments.push(String{
        allocator, spelling.substring_of_length(start, position - start)});
  }
  if (arguments.is_empty()) return None;

  let const program = resolve_transfer_program(arguments[0].view(), resolver);
  if (!program.has_value()) return None;
  arguments[0] = program->text();

  return arguments;
}

fn append_arguments(ArrayList<String> &destination,
                    const ArrayList<String> &source) throws -> void
{
  for (let const &argument : source)
    destination.push(argument.clone());
}

fn remote_command(StringView upload_name, StringView transaction_id,
                  StringView expected_identity, StringView link_moods,
                  trace_output trace, Allocator allocator) throws -> String
{
  let const should_trace = trace == trace_output::Enabled;
  let command = String{allocator};
  command += "exec ";
  let const upload_path = String{allocator, "./"} + upload_name;
  let const do_append_argument = [&](StringView argument) throws {
    append_shell_quoted_arg(command, argument);
    command += ' ';
  };
  do_append_argument(upload_path.view());
  do_append_argument("-p");
  do_append_argument("--mood");
  do_append_argument("sh");
  do_append_argument("-c");
  do_append_argument("koshkit flock --transaction-held-lock \"$@\"");
  do_append_argument("kosh");
  do_append_argument(".");
  do_append_argument(upload_path.view());
  do_append_argument("-p");
  do_append_argument("--mood");
  do_append_argument("sh");
  do_append_argument("-c");
  do_append_argument(REMOTE_TRANSACTION);
  do_append_argument("kosh");
  do_append_argument(upload_name);
  do_append_argument(transaction_id);
  do_append_argument(expected_identity);
  do_append_argument(link_moods);
  do_append_argument(should_trace ? StringView{"1"} : StringView{"0"});
  command.pop_back();

  return command;
}

fn preflight_command(StringView executable_system,
                     StringView executable_machine, trace_output trace,
                     Allocator allocator) throws -> String
{
  let command = String{allocator, "set -- "};
  append_shell_quoted_arg(command, trace == trace_output::Enabled
                                       ? StringView{"1"}
                                       : StringView{"0"});
  command += ' ';
  append_shell_quoted_arg(command, executable_system);
  command += ' ';
  append_shell_quoted_arg(command, executable_machine);
  command += "; ";
  command += REMOTE_PREFLIGHT;

  return command;
}

} /* namespace */

Assimilate::Assimilate() = default;

pure fn Assimilate::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Assimilate;
}

fn Assimilate::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  let const arguments = PARSE_BUILTIN_ARGS(ec);
  if (FLAG_HELP.is_enabled()) SHOW_BUILTIN_HELP_AND_RETURN(ec);

  bool has_invalid_target_character = false;
  bool is_inside_host_literal = false;
  bool has_host_literal = false;
  usize host_literal_start = 0;
  if (arguments.count() == 2) {
    for (usize character_position = 0;
         character_position < arguments[1].count(); character_position++)
    {
      const char character = arguments[1][character_position];
      switch (character) {
      case ' ':
      case '\t':
      case '\r':
      case '\n': has_invalid_target_character = true; break;
      case '[':
        if (has_host_literal || is_inside_host_literal ||
            (character_position != 0 &&
             arguments[1][character_position - 1] != '@'))
        {
          has_invalid_target_character = true;
        }
        has_host_literal = true;
        is_inside_host_literal = true;
        host_literal_start = character_position;
        break;
      case ']':
        if (!is_inside_host_literal ||
            character_position == host_literal_start + 1 ||
            character_position + 1 != arguments[1].count())
        {
          has_invalid_target_character = true;
        }
        is_inside_host_literal = false;
        break;
      case ':':
        if (!is_inside_host_literal) has_invalid_target_character = true;
        break;
      default: break;
      }
    }
  }
  if (is_inside_host_literal) has_invalid_target_character = true;

  if (arguments.count() != 2 || arguments[1].is_empty() ||
      arguments[1][0] == '-' || has_invalid_target_character)
  {
    return report_usage_error(ec, cxt, ec.program());
  }

  let const executable = os::current_executable_path();
  if (!executable.has_value()) {
    report_soft_builtin_error(ec, cxt,
                              "Cannot resolve this shell's executable path");
    return 1;
  }

  let const allocator = cxt.scratch_allocator();
  let const trace =
      FLAG_TRACE.is_enabled() ? trace_output::Enabled : trace_output::Disabled;
  let const scp_command = parse_transport_command(
      FLAG_SCP_COMMAND.is_set() ? FLAG_SCP_COMMAND.value() : StringView{"scp"},
      cxt.get_program_resolver(), allocator);
  let const ssh_command = parse_transport_command(
      FLAG_SSH_COMMAND.is_set() ? FLAG_SSH_COMMAND.value() : StringView{"ssh"},
      cxt.get_program_resolver(), allocator);
  if (!scp_command.has_value() || !ssh_command.has_value()) {
    report_soft_builtin_error(ec, cxt, "Cannot find both scp and ssh in PATH");
    return 127;
  }

  constexpr static_string_entry<bool> LINK_MOOD_ENTRIES[] = {
      {SSK("bash"), true},
      {SSK("dash"), true},
      {SSK("sh"),   true},
      {SSK("kosh"), true},
  };
  constexpr StaticStringMap LINK_MOODS{LINK_MOOD_ENTRIES};
  let link_moods = String{allocator};
  for (usize flag_position = 0; flag_position < FLAG_LINK_MOOD.count();
       flag_position++)
  {
    let const spelling = FLAG_LINK_MOOD.get(flag_position);
    usize mood_start = 0;
    while (mood_start <= spelling.count()) {
      let const comma = spelling.substring(mood_start).find_character(',');
      let const mood_length =
          comma.has_value() ? *comma : spelling.count() - mood_start;
      let const mood = spelling.substring_of_length(mood_start, mood_length);
      if (mood.is_empty() || !LINK_MOODS.find(mood).has_value())
        return report_usage_error(ec, cxt, ec.program());
      if (!link_moods.is_empty()) link_moods += ',';
      link_moods += mood;
      if (!comma.has_value()) break;
      mood_start += mood_length + 1;
    }
  }

  let const expected_identity = cxt.materialize_kosh_identity();
  if (!expected_identity.has_value()) {
    report_soft_builtin_error(ec, cxt,
                              "Cannot identify this shell's executable");
    return 1;
  }

  let const executable_system = os::executable_system_name();
  let const executable_machine = os::executable_machine_name();
  let preflight_arguments = ArrayList<String>{allocator};
  append_arguments(preflight_arguments, *ssh_command);
  preflight_arguments.push(String{"--"});
  preflight_arguments.push(arguments[1].clone());
  preflight_arguments.push(preflight_command(
      executable_system.view(), executable_machine.view(), trace, allocator));
  let status = run_program(steal(preflight_arguments), ec, trace, "preflight");
  if (status != 0) return status;

  let const transaction_id =
      String::from(static_cast<u64>(os::get_shell_process_id()), allocator) +
      "-" + String::from(os::realtime_microseconds(), allocator);
  let const upload_name =
      String{".kosh-assimilate-"} + transaction_id + ".upload";
  let const destination = arguments[1].view() + ":" + upload_name;

  let scp_arguments = ArrayList<String>{allocator};
  append_arguments(scp_arguments, *scp_command);
  scp_arguments.push(String{"-p"});
  scp_arguments.push(String{"--"});
  scp_arguments.push(String{*executable});
  scp_arguments.push(String{destination.view()});
  status = run_program(steal(scp_arguments), ec, trace, "upload");
  if (status != 0) return status;

  let ssh_arguments = ArrayList<String>{allocator};
  append_arguments(ssh_arguments, *ssh_command);
  ssh_arguments.push(String{"--"});
  ssh_arguments.push(arguments[1].clone());
  ssh_arguments.push(remote_command(upload_name.view(), transaction_id.view(),
                                    expected_identity->view(),
                                    link_moods.view(), trace, allocator));

  status = run_program(steal(ssh_arguments), ec, trace, "install");

  return status;
}

} /* namespace koshka */
