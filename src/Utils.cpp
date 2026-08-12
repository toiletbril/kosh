#include "Utils.hpp"

#include "Builtin.hpp"
#include "Cli.hpp"
#include "Containers.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Koshkit.hpp"
#include "Lexer.hpp"
#include "Platform.hpp"
#include "Toiletline.hpp"
#include "Trace.hpp"

namespace koshka {

namespace utils {

static fn shell_word_expansion_end(StringView word,
                                   usize expansion_start) wontthrow -> usize
{
  if (expansion_start >= word.length) return expansion_start;

  if (word[expansion_start] == '`') {
    usize position = expansion_start + 1;
    while (position < word.length) {
      if (word[position] == '\\' && position + 1 < word.length) {
        position += 2;
        continue;
      }
      if (word[position] == '`') return position + 1;
      position++;
    }
    return word.length;
  }

  if (word[expansion_start] != '$' || expansion_start + 1 >= word.length)
    return expansion_start;

  let const next_byte = word[expansion_start + 1];
  if (next_byte == '{') {
    usize depth = 1;
    usize position = expansion_start + 2;
    while (position < word.length) {
      if (word[position] == '\\' && position + 1 < word.length) {
        position += 2;
        continue;
      }
      if (word[position] == '{') depth++;
      if (word[position] == '}' && --depth == 0) return position + 1;
      position++;
    }
    return word.length;
  }

  if (next_byte == '(') {
    usize depth = 0;
    char quote_character = 0;
    for (usize position = expansion_start + 1; position < word.length;
         position++)
    {
      let const byte = word[position];
      if (byte == '\\' && quote_character != '\'' && position + 1 < word.length)
      {
        position++;
        continue;
      }
      if ((byte == '\'' || byte == '"') && quote_character == 0) {
        quote_character = byte;
        continue;
      }
      if (byte == quote_character) {
        quote_character = 0;
        continue;
      }
      if (quote_character != 0) continue;
      if (byte == '(') depth++;
      if (byte == ')' && --depth == 0) return position + 1;
    }
    return word.length;
  }

  usize position = expansion_start + 1;
  if ((next_byte >= 'a' && next_byte <= 'z') ||
      (next_byte >= 'A' && next_byte <= 'Z') || next_byte == '_')
  {
    position++;
    while (position < word.length) {
      let const byte = word[position];
      if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '_'))
      {
        break;
      }
      position++;
    }
    return position;
  }

  if ((next_byte >= '0' && next_byte <= '9') || next_byte == '@' ||
      next_byte == '*' || next_byte == '#' || next_byte == '?' ||
      next_byte == '-' || next_byte == '$' || next_byte == '!')
  {
    return expansion_start + 2;
  }

  return expansion_start;
}

fn decode_shell_word(StringView word, Allocator allocator,
                     bool should_map_source) throws -> decoded_shell_word
{
  let decoded = decoded_shell_word{allocator};
  if (should_map_source) decoded.raw_positions.push(0);

  char quote_character = 0;
  let is_scanning_tilde_prefix = false;
  let is_scanning_leading_variable = false;
  let leading_variable_is_braced = false;
  for (usize position = 0; position < word.length; position++) {
    let const byte = word[position];
    if (quote_character == 0 && (byte == '\'' || byte == '"')) {
      decoded.has_shell_syntax = true;
      if (is_scanning_tilde_prefix) {
        decoded.leading_tilde_is_active = false;
        is_scanning_tilde_prefix = false;
      }
      is_scanning_leading_variable = false;
      quote_character = byte;
      decoded.open_quote_content_start = position + 1;
      decoded.open_quote_decoded_start = decoded.text.length();
      decoded.last_quote_content_start = position + 1;
      decoded.last_quote_decoded_start = decoded.text.length();
      decoded.last_quote_character = byte;
      if (should_map_source) decoded.raw_positions.back() = position + 1;
      if (!decoded.text.is_empty() &&
          os::is_directory_separator(decoded.text.back()))
        decoded.raw_directory_end = position + 1;
      continue;
    }
    if (byte == quote_character) {
      if (is_scanning_tilde_prefix) {
        decoded.leading_tilde_is_active = false;
        is_scanning_tilde_prefix = false;
      }
      is_scanning_leading_variable = false;
      quote_character = 0;
      decoded.open_quote_content_start = 0;
      decoded.open_quote_decoded_start = 0;
      if (!decoded.text.is_empty() &&
          os::is_directory_separator(decoded.text.back()))
        decoded.raw_directory_end = position + 1;
      continue;
    }
    if (byte == '\\' && quote_character != '\'' && position + 1 < word.length) {
      let const escaped_byte = word[position + 1];
      if (quote_character != '"' || escaped_byte == '$' ||
          escaped_byte == '`' || escaped_byte == '"' || escaped_byte == '\\' ||
          escaped_byte == '\n')
      {
        decoded.has_shell_syntax = true;
        if (is_scanning_tilde_prefix) {
          decoded.leading_tilde_is_active = false;
          is_scanning_tilde_prefix = false;
        }
        is_scanning_leading_variable = false;
        position++;
        if (escaped_byte == '\n') {
          if (should_map_source) decoded.raw_positions.back() = position + 1;
          if (!decoded.text.is_empty() &&
              os::is_directory_separator(decoded.text.back()))
            decoded.raw_directory_end = position + 1;
          continue;
        }
        decoded.text.push(escaped_byte);
        decoded.glob_active.push(false);
        if (should_map_source) decoded.raw_positions.push(position + 1);
        if (os::is_directory_separator(escaped_byte))
          decoded.raw_directory_end = position + 1;
        continue;
      }
    }
    if (decoded.text.is_empty()) {
      decoded.leading_tilde_is_active = byte == '~' && quote_character == 0;
      decoded.leading_variable_is_active =
          byte == '$' && quote_character != '\'';
      is_scanning_tilde_prefix = decoded.leading_tilde_is_active;
      is_scanning_leading_variable = decoded.leading_variable_is_active;
      if (is_scanning_leading_variable)
        decoded.leading_variable_expansion_end = 1;
    } else if (is_scanning_leading_variable) {
      if (decoded.text.length() == 1 && byte == '{') {
        leading_variable_is_braced = true;
        decoded.leading_variable_expansion_end = decoded.text.length() + 1;
      } else if (lexer::is_variable_name(byte)) {
        decoded.leading_variable_expansion_end = decoded.text.length() + 1;
      } else if (leading_variable_is_braced && byte == '}') {
        decoded.leading_variable_expansion_end = decoded.text.length() + 1;
        is_scanning_leading_variable = false;
      } else {
        is_scanning_leading_variable = false;
      }
    }
    decoded.text.push(byte);
    decoded.glob_active.push(quote_character == 0 &&
                             (byte == '*' || byte == '?' || byte == '['));
    if (should_map_source) decoded.raw_positions.push(position + 1);
    if (os::is_directory_separator(byte)) {
      decoded.raw_directory_end = position + 1;
      is_scanning_tilde_prefix = false;
    }
  }
  decoded.quote_character = quote_character;

  if (!should_map_source) return decoded;

  char scan_quote = 0;
  usize decoded_position = 0;
  for (usize raw_start = 0; raw_start < word.length; raw_start++) {
    let const byte = word[raw_start];
    if (scan_quote == 0 && (byte == '\'' || byte == '"')) {
      scan_quote = byte;
      continue;
    }
    if (byte == scan_quote) {
      scan_quote = 0;
      continue;
    }
    if (byte == '\\' && scan_quote != '\'' && raw_start + 1 < word.length) {
      raw_start++;
      continue;
    }

    usize raw_end = raw_start;
    if (raw_start == 0 && byte == '~' && scan_quote == 0 &&
        decoded.leading_tilde_is_active)
    {
      raw_end = raw_start + 1;
    } else if ((byte == '$' && scan_quote != '\'') ||
               (byte == '`' && scan_quote == 0))
      raw_end = shell_word_expansion_end(word, raw_start);
    if (raw_end <= raw_start) continue;

    while (decoded_position < decoded.raw_positions.count() &&
           decoded.raw_positions[decoded_position] < raw_start)
      decoded_position++;
    let const decoded_start = decoded_position;
    while (decoded_position < decoded.raw_positions.count() &&
           decoded.raw_positions[decoded_position] < raw_end)
      decoded_position++;
    usize decoded_end = decoded_position;
    if (decoded_end > decoded.text.length())
      decoded_end = decoded.text.length();

    decoded.opaque_ranges.push(
        opaque_shell_word_range{decoded_start, decoded_end - decoded_start,
                                raw_start, raw_end - raw_start});
    raw_start = raw_end - 1;
  }

  return decoded;
}

struct path_source_component
{
  StringView text;
  usize decoded_start;
  usize decoded_end;
  bool is_opaque;
};

struct path_component_owner_range
{
  usize first_component_index;
  usize end_component_index;
};

static fn split_path_source_components(StringView path,
                                       const decoded_shell_word *decoded,
                                       Allocator allocator) throws
    -> ArrayList<path_source_component>
{
  let components = ArrayList<path_source_component>{allocator};
  usize position = os::path_root_length(path);
  if (position == 0 && os::path_is_drive_relative(path)) position = 2;
  usize opaque_range_position = 0;
  while (position < path.length) {
    while (position < path.length && os::is_directory_separator(path[position]))
      position++;
    if (position >= path.length) break;

    let const start = position;
    while (position < path.length &&
           !os::is_directory_separator(path[position]))
      position++;

    let is_opaque = false;
    if (decoded != nullptr) {
      while (opaque_range_position < decoded->opaque_ranges.count()) {
        let const &range = decoded->opaque_ranges[opaque_range_position];
        let const range_end = range.decoded_start + range.decoded_length;
        if (range_end <= start) {
          opaque_range_position++;
          continue;
        }
        if (range.decoded_start < position && range_end > start) {
          is_opaque = true;
        }
        break;
      }
    }

    components.push(
        path_source_component{path.substring_of_length(start, position - start),
                              start, position, is_opaque});
  }
  return components;
}

static fn path_component_owner(
    const ArrayList<path_source_component> &raw_components,
    const ArrayList<path_source_component> &expanded_components,
    usize expanded_component_index) wontthrow
    -> Maybe<path_component_owner_range>
{
  usize raw_left_index = 0;
  usize expanded_left_index = 0;
  while (raw_left_index < raw_components.count() &&
         expanded_left_index < expanded_components.count() &&
         !raw_components[raw_left_index].is_opaque &&
         raw_components[raw_left_index].text ==
             expanded_components[expanded_left_index].text)
  {
    if (expanded_left_index == expanded_component_index)
      return path_component_owner_range{raw_left_index, raw_left_index + 1};
    raw_left_index++;
    expanded_left_index++;
  }

  usize raw_right_index = raw_components.count();
  usize expanded_right_index = expanded_components.count();
  while (raw_right_index > raw_left_index &&
         expanded_right_index > expanded_left_index &&
         !raw_components[raw_right_index - 1].is_opaque &&
         raw_components[raw_right_index - 1].text ==
             expanded_components[expanded_right_index - 1].text)
  {
    if (expanded_right_index - 1 == expanded_component_index)
      return path_component_owner_range{raw_right_index - 1, raw_right_index};
    raw_right_index--;
    expanded_right_index--;
  }

  if (expanded_component_index >= expanded_left_index &&
      expanded_component_index < expanded_right_index &&
      raw_left_index < raw_right_index)
  {
    return path_component_owner_range{raw_left_index, raw_right_index};
  }
  return None;
}

fn locate_first_unavailable_path_component(const Path &target,
                                           StringView expanded_operand,
                                           StringView raw_operand,
                                           SourceLocation operand_location,
                                           Allocator allocator) throws
    -> Maybe<unavailable_path_source_component>
{
  let const unavailable = target.first_unavailable_component();
  if (!unavailable.has_value()) return None;

  let const decoded = decode_shell_word(raw_operand, allocator, true);
  let const raw_components =
      split_path_source_components(decoded.text.view(), &decoded, allocator);
  let const expanded_components =
      split_path_source_components(expanded_operand, nullptr, allocator);
  const usize remaining_component_count =
      unavailable->component_count - unavailable->component_index - 1;
  const usize expanded_component_index =
      expanded_components.count() > remaining_component_count
          ? expanded_components.count() - remaining_component_count - 1
          : 0;
  let const owner = path_component_owner(raw_components, expanded_components,
                                         expanded_component_index);

  let reported_prefix = String{allocator};
  if (expanded_component_index < expanded_components.count()) {
    reported_prefix.append(expanded_operand.substring_of_length(
        0, expanded_components[expanded_component_index].decoded_end));
  } else
    reported_prefix.append(expanded_operand);
  let typed_prefix = String{allocator};
  usize typed_component_start = 0;
  bool has_single_raw_component = false;
  if (owner.has_value()) {
    let const &first_component = raw_components[owner->first_component_index];
    let const &last_component = raw_components[owner->end_component_index - 1];
    typed_prefix.append(
        decoded.text.view().substring_of_length(0, last_component.decoded_end));
    operand_location.position +=
        decoded.raw_positions[first_component.decoded_start];
    operand_location.length =
        decoded.raw_positions[last_component.decoded_end] -
        decoded.raw_positions[first_component.decoded_start];
    typed_component_start = first_component.decoded_start;
    has_single_raw_component =
        owner->end_component_index == owner->first_component_index + 1;
  } else
    typed_prefix.append(expanded_operand);

  let const prefix = Path{
      target.text().view().substring_of_length(0, unavailable->component_end)};
  return unavailable_path_source_component{prefix,
                                           operand_location,
                                           steal(reported_prefix),
                                           steal(typed_prefix),
                                           typed_component_start,
                                           unavailable->is_not_directory,
                                           has_single_raw_component};
}

fn file_content_identity(const Path &path, Allocator allocator) throws
    -> Maybe<String>
{
  let const file =
      os::open_file_descriptor(path.text().view(), os::file_open_mode::Read);
  if (!file.has_value()) return None;
  defer { os::close_fd(*file); };

  u32 crc = 0xFFFFFFFF;
  char buffer[65536];
  loop
  {
    let const read_count = os::read_fd(*file, buffer, sizeof(buffer));
    if (!read_count.has_value()) return None;
    if (*read_count == 0) break;
    crc = os::crc32c_update(crc, buffer, *read_count);
  }

  crc = ~crc;
  constexpr char HEX_DIGITS[] = "0123456789abcdef";
  let digest = String{allocator};
  digest.reserve(8);
  for (usize position = 0; position < 8; position++) {
    let const shift = static_cast<u32>((7 - position) * 4);
    digest.push(HEX_DIGITS[(crc >> shift) & 0xf]);
  }
  return digest;
}

namespace {

fn compute_kosh_identity(StringView fallback_path) throws -> Maybe<String>
{
  if (let const executable = os::current_executable_path();
      executable.has_value())
  {
    let const identity =
        file_content_identity(Path{executable->view()}, heap_allocator());
    if (identity.has_value()) {
      os::set_environment_variable("KOSH_IDENTITY", identity->view());
      return identity;
    }
  }
  let const identity =
      file_content_identity(Path{fallback_path}, heap_allocator());
  if (identity.has_value())
    os::set_environment_variable("KOSH_IDENTITY", identity->view());
  return identity;
}

} /* namespace */

fn kosh_identity(StringView fallback_path) throws -> Maybe<StringView>
{
  static const Maybe<String> cached = compute_kosh_identity(fallback_path);
  if (cached.has_value()) return cached->view();
  return None;
}

fn merge_tokens_to_string(const ArrayList<const Token *> &tokens) throws
    -> String
{
  let result = String{heap_allocator()};
  usize total_length = 0;
  for (usize i = 0; i < tokens.count(); i++) {
    let const token = tokens[i];
    ASSERT(token != nullptr);
    total_length += token->raw_string().count();
    if (i + 1 < tokens.count()) total_length++;
  }
  result.reserve(total_length);
  for (usize i = 0; i < tokens.count(); i++) {
    let const token = tokens[i];
    ASSERT(token != nullptr);
    result += token->raw_string();
    if (i + 1 < tokens.count()) {
      result += ' ';
    }
  }
  return result;
}

fn execute_context(ExecContext &&ec, EvalContext &cxt,
                   execution_mode mode) throws -> i32
{
  let const is_async = mode == execution_mode::Background;
  if (ec.is_builtin()) {
    LOG(Debug, "dispatching the builtin '%s'", ec.program().c_str());
    return execute_builtin(steal(ec), cxt);
  }

  /* The terminal external command may replace the shell in place when it is the
     last command, not in a subshell, and no EXIT trap is pending. */
  let const can_replace_shell =
      cxt.terminal_exec_allowed() && !cxt.in_subshell() && !cxt.has_exit_trap();

  /* Mimicry runs the script in-process, a background command keeps its fork.
   */
  if (cxt.mimicry() && !is_async) {
    if (Maybe<mimic_mood> mode = ec.program_path().detect_mimic_shell();
        mode.has_value())
    {
      LOG(Debug, "execute_context mimicking the shell for '%s'",
          ec.program().c_str());
      if (cxt.shell_is_interactive() && os::shell_has_controlling_terminal()) {
        let const command = String{ec.program().view()};

        /* The child blocks on this pipe until the parent hands off the
           terminal, so it never touches the terminal before the handoff. */
        let const sync_pipe = os::make_pipe();

        koshka::flush();
        let const forked_child = os::try_fork_job_process();
        if (forked_child.has_value()) {
          const os::process child = *forked_child;
          if (os::process_id_of(child) == 0) {
            if (sync_pipe.has_value()) {
              /* The child drops its write end so the read unblocks on EOF if
                 the parent dies before the handoff. */
              os::close_fd(sync_pipe->out);
              char handoff_byte = 0;
              (void) os::read_fd(sync_pipe->in, &handoff_byte, 1);
              os::close_fd(sync_pipe->in);
            }
            i32 status = 1;
            try {
              status =
                  cxt.run_mimicked_script(ec, *mode, script_isolation::Shared);
            } catch (const ErrorBase &error) {
              const String *source = cxt.current_source();
              show_message(error.to_string(
                  source != nullptr ? source->view() : StringView{}, &cxt));
              status = static_cast<i32>(error.command_status());
            } catch (...) {}
            os::exit_process_immediately(status);
          }

          if (sync_pipe.has_value()) {
            os::close_fd(sync_pipe->in);
          }
          os::give_controlling_terminal_to(child);
          if (sync_pipe.has_value()) {
            (void) os::write_fd(sync_pipe->out, "x", 1);
            os::close_fd(sync_pipe->out);
          }

          let was_stopped = false;
          const i32 status = os::wait_and_monitor_process(child, &was_stopped);
          os::reclaim_controlling_terminal();

          if (was_stopped) {
            const i32 id = cxt.register_stopped_job(child, command, status,
                                                    os::process_id_of(child));
            cxt.notify_stopped_job(id, command.view());
          }
          return status;
        }

        if (sync_pipe.has_value()) {
          os::close_fd(sync_pipe->in);
          os::close_fd(sync_pipe->out);
        }
      }
      return cxt.run_mimicked_script(ec, *mode,
                                     can_replace_shell
                                         ? script_isolation::Shared
                                         : script_isolation::Isolated);
    }
  }

  /* The terminal external command replaces the shell in place, the way dash
     execs the last command under EV_EXIT. The EXIT trap is rechecked at run
     time here, since one set earlier in this chunk must still run. */
  if (!is_async && can_replace_shell) {
    LOG(Debug,
        "execute_context replacing the shell with the terminal command '%s'",
        ec.program().c_str());
    flush();
    unused(cxt.materialize_kosh_identity());
    try {
      os::replace_process(steal(ec));
    } catch (const ErrorWithLocation &error) {
      /* Resolved but unexecutable exits 126, missing exits 127. */
      const String *source = cxt.current_source();
      show_message(error.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      quit(126, farewell_policy::Silent);
    } catch (const Error &error) {
      const String *source = cxt.current_source();
      let located = ErrorWithLocation{ec.source_location(), error.message()};
      located.set_command_status(error.command_status());
      show_message(located.to_string(
          source != nullptr ? source->view() : StringView{}, &cxt));
      quit(127, farewell_policy::Silent);
    }
    LOG(Debug, "running the file as a shell script in place");
    ec.in_fd.reset();
    ec.out_fd.reset();
    ec.err_fd.reset();
    const mimic_mood mode = cxt.mood();
    quit(cxt.run_program_fallback(ec, mode,
                                  can_replace_shell
                                      ? script_isolation::Shared
                                      : script_isolation::Isolated),
         farewell_policy::Silent);
  }

  LOG(Debug, "spawning the external command '%s'%s", ec.program().c_str(),
      is_async ? " in the background" : "");

  /* An interactive foreground command runs in its own process group and holds
     the terminal, so it dies on its own Ctrl-C. */
  const bool is_foreground_job = !is_async && cxt.shell_is_interactive() &&
                                 os::shell_has_controlling_terminal();

  let command = String{heap_allocator()};
  if (is_async || is_foreground_job) {
    for (usize i = 0; i < ec.args().count(); i++) {
      if (i > 0) command += ' ';
      command += ec.args()[i].view();
    }
    if (is_async) command += " &";
  }

  let const source = cxt.current_source();
  unused(cxt.materialize_kosh_identity());
  os::process p =
      os::execute_program(steal(ec),
                          is_async ? os::script_fallback_policy::Reject
                                   : os::script_fallback_policy::Allow,
                          is_async ? os::process_group_mode::NewBackground
                          : is_foreground_job ? os::process_group_mode::New
                                              : os::process_group_mode::Inherit,
                          source != nullptr ? source->view() : StringView{});
  if (p == KOSH_INVALID_PROCESS) {
    LOG(Debug, "running the file as a shell script in this process");
    const mimic_mood mode = cxt.mood();
    return cxt.run_program_fallback(ec, mode,
                                    can_replace_shell
                                        ? script_isolation::Shared
                                        : script_isolation::Isolated);
  }
  if (is_async) {
    cxt.set_last_background_pid(os::process_id_of(p));
    let const process_group_id = os::process_id_of(p);
    const i32 id = cxt.register_job(p, command, process_group_id);
    if (cxt.shell_is_interactive())
      koshka::print_error("[" + String::from(id, heap_allocator()) + "] " +
                          String::from(static_cast<u64>(os::process_id_of(p)),
                                       heap_allocator()) +
                          "\n");
    return 0;
  }

  LOG(Debug, "waiting for the foreground child to finish");
  let should_reclaim_after_wait = is_foreground_job;
  if (should_reclaim_after_wait) os::give_controlling_terminal_to(p);
  defer
  {
    if (should_reclaim_after_wait) os::reclaim_controlling_terminal();
  };
  let was_stopped = false;
  const i32 foreground_status = os::wait_and_monitor_process(
      p, is_foreground_job ? &was_stopped : nullptr);
  if (should_reclaim_after_wait) {
    os::reclaim_controlling_terminal();
    should_reclaim_after_wait = false;
  }
  if (was_stopped) {
    const i32 id = cxt.register_stopped_job(p, command, foreground_status,
                                            os::process_id_of(p));
    cxt.notify_stopped_job(id, command.view());
  }
  return foreground_status;
}

pure static fn builtin_can_launch_fresh(Builtin::Kind kind) wontthrow -> bool
{
  switch (kind) {
  case Builtin::Kind::Echo:
  case Builtin::Kind::Pwd:
  case Builtin::Kind::True:
  case Builtin::Kind::False:
  case Builtin::Kind::Test:
  case Builtin::Kind::Printf:
  case Builtin::Kind::Koshkit: return true;
  default: return false;
  }
}

fn terminate_and_reap_processes(const ArrayList<os::process> &processes,
                                usize first_process_position) wontthrow -> void
{
  for (usize position = first_process_position; position < processes.count();
       position++)
    unused(os::signal_process(processes[position], 9));

  for (usize position = first_process_position; position < processes.count();
       position++)
  {
    try {
      os::wait_and_monitor_process(processes[position]);
    } catch (...) {}
  }
}

fn execute_contexts_with_pipes(ArrayList<ExecContext> &&ecs, EvalContext &cxt,
                               execution_mode mode) throws -> i32
{
  let const is_async = mode == execution_mode::Background;
  ASSERT(ecs.count() > 1);

  LOG(Debug, "running a pipeline of %zu stages%s", ecs.count(),
      is_async ? " in the background" : "");

  i32 ret = 0;

  /* Every external stage is collected so all of them are reaped, not only the
     last. Otherwise a first stage like yes is left a zombie when the last stage
     exits. */
  let children = ArrayList<os::process>{heap_allocator()};
  os::process last_child = KOSH_INVALID_PROCESS;
  os::descriptor last_stdin = KOSH_INVALID_FD;
  i64 process_group_id = 0;
  bool should_reap_children_on_unwind = true;
  defer
  {
    if (should_reap_children_on_unwind) {
      for (ExecContext &pending_context : ecs)
        pending_context.close_fds();
      if (last_stdin != KOSH_INVALID_FD) os::close_fd(last_stdin);
      terminate_and_reap_processes(children);
    }
  };

  /* Each stage's status is recorded against its position, so pipefail can
     report the rightmost stage that failed and the plain case can read the last
     stage. A builtin stage yields its status at once and an external one's
     status arrives from the wait below, tracked by the parallel child-to-stage
     list. */
  let const stage_count = ecs.count();
  let stage_status = ArrayList<i32>{heap_allocator()};
  stage_status.reserve(stage_count);
  for (usize i = 0; i < stage_count; i++)
    stage_status.push(0);
  let child_stage = ArrayList<usize>{heap_allocator()};

  bool is_first = true;
  usize stage_index = 0;

  for (ExecContext &ec : ecs) {
    Maybe<os::Pipe> pipe;

    let const is_last = (&ec == &ecs.back());

    if (!is_last) {
      pipe = os::make_pipe();
      if (!pipe) {
        throw ErrorWithLocation{ec.source_location(), "Could not open a pipe"};
      }
      /* An explicit > takes the stage's stdout, so the pipe end closes unused.
       */
      if (!ec.out_fd)
        ec.out_fd = pipe->out;
      else
        os::close_fd(pipe->out);
    }

    if (!is_first) {
      if (!ec.in_fd)
        ec.in_fd = last_stdin;
      else
        os::close_fd(last_stdin);
    }
    if (!is_last) {
      last_stdin = pipe->in;
    }

    if (ec.is_unresolved()) {
      stage_status[stage_index] = ec.get_unresolved_status();
      ec.close_fds();
    } else if (!ec.is_builtin()) {
      let const source = cxt.current_source();
      unused(cxt.materialize_kosh_identity());
      let const process_group =
          !is_async ? os::process_group_mode::Inherit
                    : os::background_process_group_mode(process_group_id);
      let const child = os::execute_program(
          steal(ec), os::script_fallback_policy::Reject, process_group,
          source != nullptr ? source->view() : StringView{},
          os::terminal_handoff::Keep, process_group_id);
      if (is_async && process_group_id == 0)
        process_group_id = os::process_id_of(child);
      children.push(child);
      child_stage.push(stage_index);
      last_child = child;
    } else if (!is_last || is_async) {
      let const source = cxt.current_source();
      let const process_group =
          !is_async ? os::process_group_mode::Inherit
                    : os::background_process_group_mode(process_group_id);
      let forked_child = os::try_fork_compound_stage(
          ec.in_fd, ec.out_fd, ec.err_fd, ec.source_location(),
          source != nullptr ? source->view() : StringView{}, process_group,
          process_group_id);
      let preflight_status = Maybe<i32>{};
      let preflight_location = SourceLocation{};
      let preflight_message = String{cxt.scratch_allocator()};
      if (!forked_child.has_value() &&
          builtin_can_launch_fresh(ec.builtin_kind()))
      {
        const usize utility_index = ec.program() == "koshkit" ? 1 : 0;
        bool should_launch_fresh_stage =
            ec.builtin_kind() != Builtin::Kind::Koshkit;
        bool should_restore_environment = false;
        if (ec.builtin_kind() == Builtin::Kind::Koshkit &&
            utility_index < ec.args().count())
        {
          let const utility_kind =
              koshkit::find_util(ec.args()[utility_index].view());
          if (utility_kind.has_value()) {
            should_launch_fresh_stage = true;
            if (!is_async && *utility_kind == koshkit::Utility::Kind::Timeout) {
              preflight_status = koshkit::preflight_timeout_stage(
                  ec, cxt, utility_index, preflight_location,
                  preflight_message);
            }
            should_restore_environment =
                *utility_kind == koshkit::Utility::Kind::Env;
          }
        }

        if (should_launch_fresh_stage && !preflight_status.has_value()) {
          let stage_source = String{cxt.scratch_allocator()};
          if (should_restore_environment) {
            static const StringView RESTORED_ENVIRONMENT_NAMES[] = {
                "PWD",
                "KOSH",
                "KOSH_VERSION",
                "KOSH_COMMIT",
                "KOSH_BUILD_MODE",
                "KOSH_OS",
                "BASH_VERSION",
                "BASH",
                "SHLVL",
                "PATH",
                "NO_COLOR",
                "KOSH_INTERNAL_SUPPRESS_ROOT_TRACE"};
            for (let const name : RESTORED_ENVIRONMENT_NAMES) {
              let const value = os::get_environment_variable(name);
              if (value.has_value()) {
                stage_source.append("export ");
                stage_source.append(name);
                stage_source.push('=');
                append_shell_quoted_arg(stage_source, value->view(), true);
              } else {
                stage_source.append("unset ");
                stage_source.append(name);
              }
              stage_source.append("; ");
            }
          }
          if (ec.builtin_kind() == Builtin::Kind::Koshkit &&
              ec.program() != "koshkit")
          {
            stage_source.append("koshkit ");
          }
          for (usize argument_index = 0; argument_index < ec.args().count();
               argument_index++)
          {
            if (argument_index > 0) stage_source.push(' ');
            append_shell_quoted_arg(stage_source, ec.args()[argument_index],
                                    true);
          }

          let stage_out = ec.out_fd;
          let stage_err = ec.err_fd;
          ec.apply_dup_routing(
              [&]() { stage_err = stage_out.value_or(KOSH_STDOUT); },
              [&]() { stage_out = stage_err.value_or(KOSH_STDERR); });
          try {
            let const launch = os::launch_compound_stage(
                stage_source.view(), ec.in_fd, stage_out, stage_err, cxt.mood(),
                ec.source_location(),
                source != nullptr ? source->view() : StringView{},
                process_group, process_group_id);
            forked_child = launch.child;
          } catch (...) {
            ec.close_fds();
            os::close_fd(last_stdin);
            last_stdin = KOSH_INVALID_FD;
            throw;
          }
        }
      }

      if (preflight_status.has_value()) {
        let const error =
            ErrorWithLocation{preflight_location, preflight_message.view()};
        let diagnostic = String{cxt.scratch_allocator()};
        diagnostic += error.to_string(
            source != nullptr ? source->view() : StringView{}, &cxt);
        diagnostic.push('\n');
        let diagnostic_out = ec.out_fd;
        let diagnostic_err = ec.err_fd;
        ec.apply_dup_routing(
            [&]() { diagnostic_err = diagnostic_out.value_or(KOSH_STDOUT); },
            [&]() { diagnostic_out = diagnostic_err.value_or(KOSH_STDERR); });
        os::signal_internal_diagnostic();
        usize written_count = 0;
        while (written_count < diagnostic.count()) {
          let const written = os::write_fd(diagnostic_err.value_or(KOSH_STDERR),
                                           diagnostic.data() + written_count,
                                           diagnostic.count() - written_count);
          if (!written.has_value() || *written == 0) {
            const i32 saved_errno = errno;
            if (saved_errno == EPIPE) throw BrokenPipeExit{};
            throw Error{"Unable to write to stderr: " +
                        os::last_system_error_message()};
          }
          written_count += *written;
        }

        stage_status[stage_index] = *preflight_status;
        ec.close_fds();
      } else if (!forked_child.has_value()) {
        cxt.set_in_pipeline_stage(true);
        defer { cxt.set_in_pipeline_stage(false); };
        ret = execute_builtin(steal(ec), cxt);
        stage_status[stage_index] = ret;
      } else {
        const os::process child = *forked_child;
        if (os::process_id_of(child) == 0) {
          ec.in_fd = koshka::None;
          ec.out_fd = koshka::None;
          ec.err_fd = koshka::None;
          if (last_stdin != KOSH_INVALID_FD) os::close_fd(last_stdin);
          cxt.set_in_pipeline_stage(true);
          cxt.enter_subshell();
          i32 child_status = 0;
          try {
            child_status = execute_builtin(steal(ec), cxt);
          } catch (const BrokenPipeExit &) {
            child_status = KOSH_BROKEN_PIPE_EXIT_STATUS;
          } catch (const ErrorWithLocation &e) {
            const String *source = cxt.current_source();
            koshka::show_message(e.to_string(
                source != nullptr ? source->view() : StringView{}, &cxt));
            child_status = static_cast<i32>(e.command_status());
          } catch (const Error &e) {
            koshka::show_message(e.to_string());
            child_status = static_cast<i32>(e.command_status());
          } catch (...) {
            child_status = 1;
          }
          koshka::flush();
          os::exit_process_immediately(child_status);
        }

        if (is_async && process_group_id == 0)
          process_group_id = os::process_id_of(child);
        ec.close_fds();
        children.push(child);
        child_stage.push(stage_index);
        last_child = child;
      }
    } else {
      /* The last builtin stage runs in this process so a cd affects the shell.
         The flag makes exec spawn a child rather than replace the shell. */
      cxt.set_in_pipeline_stage(true);
      defer { cxt.set_in_pipeline_stage(false); };
      ret = execute_builtin(steal(ec), cxt);
      stage_status[stage_index] = ret;
    }

    is_first = false;
    stage_index++;
  }

  if (is_async) {
    if (last_child != KOSH_INVALID_PROCESS) {
      cxt.set_last_background_pid(os::process_id_of(last_child));
      const i32 id = cxt.register_pipeline_job(children, last_child, "pipeline",
                                               process_group_id);
      should_reap_children_on_unwind = false;
      if (cxt.shell_is_interactive())
        koshka::print_error(
            "[" + String::from(id, heap_allocator()) + "] " +
            String::from(static_cast<u64>(os::process_id_of(last_child)),
                         heap_allocator()) +
            "\n");
    }
    return ret;
  }

  usize waited_child_count = 0;
  try {
    for (; waited_child_count < children.count(); waited_child_count++)
      stage_status[child_stage[waited_child_count]] =
          os::wait_and_monitor_process(children[waited_child_count]);
  } catch (...) {
    terminate_and_reap_processes(children, waited_child_count);
    should_reap_children_on_unwind = false;
    throw;
  }
  should_reap_children_on_unwind = false;

  let pipe_status = ArrayList<String>{heap_allocator()};
  pipe_status.reserve(stage_count);
  for (usize i = 0; i < stage_count; i++)
    pipe_status.push(String::from(stage_status[i], heap_allocator()));
  cxt.set_indexed_array("PIPESTATUS", steal(pipe_status));

  /* pipefail reports the rightmost failing stage, otherwise the last stage. */
  if (cxt.pipefail()) {
    for (usize i = stage_count; i > 0; i--)
      if (stage_status[i - 1] != 0) return stage_status[i - 1];
    return 0;
  }

  return stage_status[stage_count - 1];
}

pure fn strip_sig_prefix(StringView name) wontthrow -> StringView
{
  if (name.starts_with("SIG")) return name.substring(3);
  return name;
}

fn split_lines(StringView text) throws -> ArrayList<StringView>
{
  let lines = ArrayList<StringView>{heap_allocator()};
  usize line_count = 1;
  for (usize i = 0; i < text.length; i++)
    if (text[i] == '\n') line_count++;
  lines.reserve(line_count);

  usize line_start_position = 0;
  for (usize i = 0; i <= text.length; i++) {
    if (i != text.length && text[i] != '\n') continue;

    lines.push(
        text.substring_of_length(line_start_position, i - line_start_position));
    line_start_position = i + 1;
  }
  return lines;
}

fn format_unix_timestamp(i64 unix_time, const char *format) throws -> String
{
  time_t when = static_cast<time_t>(unix_time);
  struct tm *local = ::localtime(&when);
  if (local == nullptr) return String{heap_allocator()};

  char buffer[128];
  usize written = ::strftime(buffer, sizeof(buffer), format, local);
  return String{
      StringView{buffer, written}
  };
}

pure fn is_posix_reserved_word(StringView word) wontthrow -> bool
{
  static const StringView RESERVED_WORDS[] = {
      "!",    "{",  "}",   "case", "do", "done", "elif",  "else",
      "esac", "fi", "for", "if",   "in", "then", "until", "while",
  };
  for (let const reserved : RESERVED_WORDS)
    if (word == reserved) return true;
  return false;
}

static pure fn is_ascii_whitespace(char c) wontthrow -> bool
{
  return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
         c == '\r';
}

/* Turn an accumulated magnitude and sign into a saturating signed result. The
   per-base parsers share this so only the digit loop stays base-specific. */
static pure fn saturate_signed_magnitude(u64 magnitude, bool is_negative,
                                         bool has_overflowed) wontthrow -> i64
{
  if (is_negative) {
    if (has_overflowed || magnitude > static_cast<u64>(INT64_MAX) + 1)
      return INT64_MIN;
    return static_cast<i64>(~magnitude + 1u);
  }
  if (has_overflowed || magnitude > static_cast<u64>(INT64_MAX))
    return INT64_MAX;
  return static_cast<i64>(magnitude);
}

static fn not_an_integer_error(StringView text) throws -> Error
{
  return Error{"'" + text + "' is not a valid integer"};
}

fn int_to_text_into(i64 value, char *buffer, usize buffer_size) wontthrow
    -> StringView
{
  /* The digits are written from the least significant end of the buffer, the
     same scheme String::from uses, then a leading minus is prepended. A u64
     never needs more than twenty digits, so twenty-one bytes hold any i64. */
  ASSERT(buffer_size >= 21, "the buffer must hold a sign and twenty digits");
  const bool is_negative = value < 0;
  u64 magnitude =
      is_negative ? ~static_cast<u64>(value) + 1 : static_cast<u64>(value);
  usize offset = buffer_size;
  do {
    buffer[--offset] = static_cast<char>('0' + magnitude % 10);
    magnitude /= 10;
  } while (magnitude > 0);
  if (is_negative) buffer[--offset] = '-';
  return StringView{buffer + offset, buffer_size - offset};
}

fn format_minutes_seconds(double seconds) throws -> String
{
  /* An rusage subtraction can go backwards, a negative clamps to zero to avoid
     a doubled sign like -0m-0.001s. */
  if (seconds < 0.0) seconds = 0.0;
  const i64 minutes = static_cast<i64>(seconds) / 60;
  const double remainder = seconds - static_cast<double>(minutes * 60);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%ldm%.3fs", static_cast<long>(minutes),
                remainder);
  return String{buffer};
}

fn format_time_report_posix(double real_seconds, double user_seconds,
                            double system_seconds) throws -> String
{
  char buffer[64];
  let report = String{heap_allocator()};
  std::snprintf(buffer, sizeof(buffer), "real %.2f\n",
                real_seconds < 0.0 ? 0.0 : real_seconds);
  report += buffer;
  std::snprintf(buffer, sizeof(buffer), "user %.2f\n",
                user_seconds < 0.0 ? 0.0 : user_seconds);
  report += buffer;
  std::snprintf(buffer, sizeof(buffer), "sys %.2f\n",
                system_seconds < 0.0 ? 0.0 : system_seconds);
  report += buffer;
  return report;
}

fn format_time_report_pretty(double real_seconds, double user_seconds,
                             double system_seconds, u64 peak_rss_bytes) throws
    -> String
{
  const double cpu_percent =
      real_seconds > 0.0
          ? (user_seconds + system_seconds) / real_seconds * 100.0
          : 0.0;
  char buffer[64];
  let report = String{heap_allocator()};
  report += "\n";
  report += "  real   " + format_minutes_seconds(real_seconds) + "\n";
  report += "  user   " + format_minutes_seconds(user_seconds) + "\n";
  report += "  sys    " + format_minutes_seconds(system_seconds) + "\n";
  std::snprintf(buffer, sizeof(buffer), "  cpu    %.0f%%\n", cpu_percent);
  report += buffer;

  if (peak_rss_bytes > 0)
    report += "  rss    " +
              koshkit::format_human_size(peak_rss_bytes, heap_allocator()) +
              "\n";

  return report;
}

fn format_time_report_custom(StringView format, double real_seconds,
                             double user_seconds, double system_seconds) throws
    -> String
{
  let report = String{heap_allocator()};

  for (usize i = 0; i < format.length; i++) {
    if (format[i] != '%') {
      report.push(format[i]);
      continue;
    }

    i++;
    if (i >= format.length) {
      report.push('%');
      break;
    }
    if (format[i] == '%') {
      report.push('%');
      continue;
    }

    /* A precision digit and the l flag may precede the conversion, %3lR is
       three digits in minutes form, precision clamped to six. */
    usize precision = 3;
    if (format[i] >= '0' && format[i] <= '9') {
      precision = static_cast<usize>(format[i] - '0');
      if (precision > 6) precision = 6;
      i++;
    }

    bool is_long_format = false;
    if (i < format.length && format[i] == 'l') {
      is_long_format = true;
      i++;
    }

    if (i >= format.length) {
      report.push('%');
      break;
    }

    char buffer[64];
    let const code = format[i];

    double value = 0.0;
    switch (code) {
    case 'R': value = real_seconds; break;
    case 'U': value = user_seconds; break;
    case 'S': value = system_seconds; break;

    case 'P': {
      const double cpu_percent =
          real_seconds > 0.0
              ? (user_seconds + system_seconds) / real_seconds * 100.0
              : 0.0;
      std::snprintf(buffer, sizeof(buffer), "%.2f", cpu_percent);
      report += buffer;
      continue;
    }

    default:
      report.push('%');
      report.push(code);
      continue;
    }

    if (value < 0.0) value = 0.0;

    if (is_long_format) {
      const i64 minutes = static_cast<i64>(value) / 60;
      const double remainder = value - static_cast<double>(minutes * 60);
      std::snprintf(buffer, sizeof(buffer), "%ldm%.*fs",
                    static_cast<long>(minutes), static_cast<int>(precision),
                    remainder);
    } else {
      std::snprintf(buffer, sizeof(buffer), "%.*f", static_cast<int>(precision),
                    value);
    }
    report += buffer;
  }

  report.push('\n');
  return report;
}

/* A newline offset table cached on one source, keyed on the source pointer and
   length, so a $LINENO lookup is a binary search over the newlines. */
class LineNumberCache
{
public:
  LineNumberCache() : m_newline_offsets(heap_allocator()) {}

  fn ensure_built_for(StringView source) throws -> void
  {
    if (m_source_data == source.data && m_source_length == source.count())
      return;

    m_source_data = source.data;
    m_source_length = source.count();
    m_newline_offsets.clear();

    usize scan_position = 0;
    while (scan_position < source.count()) {
      let const remaining = source.substring(scan_position);
      let const newline = remaining.find_character('\n');
      if (!newline.has_value()) break;
      m_newline_offsets.push(scan_position + *newline);
      scan_position += *newline + 1;
    }
  }

  fn invalidate() wontthrow -> void
  {
    m_source_data = nullptr;
    m_source_length = 0;
    m_newline_offsets.clear();
  }

  pure fn locate(usize position) const wontthrow -> source_line_position
  {
    usize low = 0;
    usize high = m_newline_offsets.count();
    while (low < high) {
      const usize mid = low + (high - low) / 2;
      if (m_newline_offsets[mid] < position)
        low = mid + 1;
      else
        high = mid;
    }

    const usize line_start = low == 0 ? 0 : m_newline_offsets[low - 1] + 1;
    const usize line_end = low == m_newline_offsets.count()
                               ? m_source_length
                               : m_newline_offsets[low];
    return source_line_position{low, line_start, line_end};
  }

private:
  const char *m_source_data{nullptr};
  usize m_source_length{0};
  ArrayList<usize> m_newline_offsets;
};

static LineNumberCache LINE_NUMBER_CACHE{};

fn source_line_position_at(StringView source, usize position) throws
    -> source_line_position
{
  LINE_NUMBER_CACHE.ensure_built_for(source);
  return LINE_NUMBER_CACHE.locate(position);
}

fn line_number_at(StringView source, usize position) throws -> usize
{
  return source_line_position_at(source, position).line_number + 1;
}

fn invalidate_line_number_cache() wontthrow -> void
{
  LINE_NUMBER_CACHE.invalidate();
}

static fn skip_ascii_whitespace(StringView text, usize &offset) wontthrow
    -> void
{
  while (offset < text.length && is_ascii_whitespace(text.data[offset]))
    offset++;
}

struct parsed_integer_magnitude
{
  u64 magnitude;
  bool is_negative;
  bool has_overflowed;
};

static fn parse_decimal_magnitude(StringView text) throws
    -> ErrorOr<parsed_integer_magnitude>
{
  usize offset = 0;
  skip_ascii_whitespace(text, offset);

  bool is_negative = false;
  if (offset < text.length &&
      (text.data[offset] == '+' || text.data[offset] == '-'))
  {
    is_negative = text.data[offset] == '-';
    offset++;
  }

  u64 magnitude = 0;
  bool has_digits = false;
  bool has_overflowed = false;
  /* A u64 holds up to nineteen decimal digits with no overflow, so the first
     nineteen accumulate without the per-digit overflow division, which the hot
     arithmetic read path runs millions of times on small numbers. The division
     guard runs only once a twentieth digit can push past the type's range. */
  usize digit_count = 0;
  while (offset < text.length && text.data[offset] >= '0' &&
         text.data[offset] <= '9')
  {
    const u64 digit = static_cast<u64>(text.data[offset] - '0');
    has_digits = true;
    digit_count++;
    if (digit_count <= 19) {
      magnitude = magnitude * 10 + digit;
    } else if (magnitude > (UINT64_MAX - digit) / 10) {
      has_overflowed = true;
    } else {
      magnitude = magnitude * 10 + digit;
    }
    offset++;
  }

  skip_ascii_whitespace(text, offset);
  if (!has_digits || offset != text.length) return not_an_integer_error(text);

  return parsed_integer_magnitude{magnitude, is_negative, has_overflowed};
}

fn parse_decimal_i64(StringView text, bool *out_of_range) throws -> ErrorOr<i64>
{
  let const parsed = TRY(parse_decimal_magnitude(text));

  if (out_of_range != nullptr)
    *out_of_range = parsed.has_overflowed ||
                    parsed.magnitude > static_cast<u64>(INT64_MAX) +
                                           (parsed.is_negative ? 1u : 0u);

  return saturate_signed_magnitude(parsed.magnitude, parsed.is_negative,
                                   parsed.has_overflowed);
}

fn parse_decimal_u64(StringView text) throws -> ErrorOr<u64>
{
  let const parsed = TRY(parse_decimal_magnitude(text));
  if (parsed.is_negative || parsed.has_overflowed)
    return Error{"integer value out of range"};
  return parsed.magnitude;
}

fn parse_decimal_f64(const String &text) throws -> ErrorOr<f64>
{
  let const start = text.c_str();
  char *end = nullptr;
  errno = 0;
  let const parsed_value = ::strtold(start, &end);
  if (end == start || end != start + text.length()) {
    return Error{"invalid number"};
  }

  let digits = start;
  while (*digits == ' ' || *digits == '\t' || *digits == '\n' ||
         *digits == '\r' || *digits == '\f' || *digits == '\v')
  {
    digits++;
  }
  if (*digits == '+' || *digits == '-') {
    digits++;
  }
  if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) {
    return Error{"invalid number"};
  }

  let const magnitude = __builtin_fabsl(parsed_value);
  if (errno == ERANGE &&
      (magnitude == 0.0L || !__builtin_isfinite(parsed_value)))
  {
    return Error{"number value out of range"};
  }
  if (__builtin_isfinite(parsed_value) &&
      magnitude > static_cast<long double>(__DBL_MAX__))
  {
    return Error{"number value out of range"};
  }

  let const narrowed_value = static_cast<f64>(parsed_value);
  if (parsed_value != 0.0L && narrowed_value == 0.0)
    return Error{"number value out of range"};

  return narrowed_value;
}

fn format_f64(f64 value, Allocator allocator) throws -> String
{
  char buffer[32];
  let const length = ::snprintf(buffer, sizeof(buffer), "%.17g", value);
  if (length < 0 || static_cast<usize>(length) >= sizeof(buffer)) {
    return String{allocator};
  }

  return String{
      allocator, StringView{buffer, static_cast<usize>(length)}
  };
}

fn parse_timeout_seconds_to_nanos(StringView text) throws -> ErrorOr<i64>
{
  usize offset = 0;
  skip_ascii_whitespace(text, offset);

  u64 whole_seconds = 0;
  bool has_overflowed = false;
  bool has_digits = false;
  while (offset < text.length && text.data[offset] >= '0' &&
         text.data[offset] <= '9')
  {
    let const digit = static_cast<u64>(text.data[offset] - '0');
    if (whole_seconds > (UINT64_MAX - digit) / 10)
      has_overflowed = true;
    else
      whole_seconds = whole_seconds * 10 + digit;
    has_digits = true;
    offset++;
  }

  i64 fractional_nanos = 0;
  if (offset < text.length && text.data[offset] == '.') {
    offset++;
    i64 digit_scale = 100'000'000;
    while (offset < text.length && text.data[offset] >= '0' &&
           text.data[offset] <= '9')
    {
      fractional_nanos += (text.data[offset] - '0') * digit_scale;
      digit_scale /= 10;
      has_digits = true;
      offset++;
    }
  }

  skip_ascii_whitespace(text, offset);
  if (!has_digits || offset != text.length)
    return Error{"'" + text + "' is not a valid timeout"};

  /* A whole-seconds part too large for the signed nanosecond result saturates
     to the maximum rather than overflowing. */
  constexpr u64 max_whole_seconds = INT64_MAX / 1'000'000'000;
  constexpr i64 max_fractional_nanos = INT64_MAX % 1'000'000'000;
  if (has_overflowed || whole_seconds > max_whole_seconds ||
      (whole_seconds == max_whole_seconds &&
       fractional_nanos > max_fractional_nanos))
  {
    return static_cast<i64>(INT64_MAX);
  }

  return static_cast<i64>(whole_seconds) * 1'000'000'000 + fractional_nanos;
}

static pure fn digit_value_in_base(char c, u32 radix) wontthrow -> i32
{
  u32 value;
  if (c >= '0' && c <= '9')
    value = static_cast<u32>(c - '0');
  else if (c >= 'a' && c <= 'z')
    value = static_cast<u32>(c - 'a') + 10;
  else if (c >= 'A' && c <= 'Z')
    value = static_cast<u32>(c - 'A') + 10;
  else
    return -1;

  return value < radix ? static_cast<i32>(value) : -1;
}

static fn parse_magnitude_in_base(StringView text, int_base base) throws
    -> ErrorOr<parsed_integer_magnitude>
{
  let const radix = static_cast<u32>(base);
  usize offset = 0;
  skip_ascii_whitespace(text, offset);

  bool is_negative = false;
  if (offset < text.length &&
      (text.data[offset] == '+' || text.data[offset] == '-'))
  {
    is_negative = text.data[offset] == '-';
    offset++;
  }

  if (base == int_base::hex && offset + 1 < text.length &&
      text.data[offset] == '0' &&
      (text.data[offset + 1] == 'x' || text.data[offset + 1] == 'X'))
  {
    offset += 2;
  } else if (base == int_base::binary && offset + 1 < text.length &&
             text.data[offset] == '0' &&
             (text.data[offset + 1] == 'b' || text.data[offset + 1] == 'B'))
  {
    offset += 2;
  }

  u64 magnitude = 0;
  bool has_digits = false;
  bool has_overflowed = false;
  while (offset < text.length) {
    let const digit = digit_value_in_base(text.data[offset], radix);
    if (digit < 0) break;
    has_digits = true;
    if (magnitude > (UINT64_MAX - static_cast<u64>(digit)) / radix)
      has_overflowed = true;
    else
      magnitude = magnitude * radix + static_cast<u64>(digit);
    offset++;
  }

  skip_ascii_whitespace(text, offset);
  if (!has_digits || offset != text.length) return not_an_integer_error(text);

  return parsed_integer_magnitude{magnitude, is_negative, has_overflowed};
}

fn parse_integer_in_base(StringView text, int_base base,
                         bool *out_of_range) throws -> ErrorOr<i64>
{
  let const parsed = TRY(parse_magnitude_in_base(text, base));

  if (out_of_range != nullptr)
    *out_of_range = parsed.has_overflowed ||
                    parsed.magnitude > static_cast<u64>(INT64_MAX) +
                                           (parsed.is_negative ? 1u : 0u);

  return saturate_signed_magnitude(parsed.magnitude, parsed.is_negative,
                                   parsed.has_overflowed);
}

fn parse_integer_in_base_u64(StringView text, int_base base) throws
    -> ErrorOr<u64>
{
  let const parsed = TRY(parse_magnitude_in_base(text, base));
  if (parsed.is_negative || parsed.has_overflowed)
    return Error{"integer value out of range"};
  return parsed.magnitude;
}

fn expand_leading_tilde_path(StringView name) throws -> Maybe<String>
{
  if (name.is_empty() || name[0] != '~') return None;

  let const slash = name.find_character('/');
  let const user = slash.has_value() ? name.substring_of_length(1, *slash - 1)
                                     : name.substring(1);
  Maybe<Path> home =
      user.is_empty() ? os::get_home_directory() : os::get_home_for_user(user);
  if (!home.has_value()) return None;

  let expanded = *home;
  if (slash.has_value()) expanded.push_component(name.substring(*slash + 1));
  return String{expanded.text().view()};
}

fn decode_ansi_c_escapes(String &out, StringView body) throws -> void
{
  let const do_hex_value = [](char h) -> i32 {
    if (h >= '0' && h <= '9') return h - '0';
    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
    return -1;
  };

  let do_emit_codepoint = [&](u32 cp) throws {
    if (cp < 0x80) {
      out.push(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push(static_cast<char>(0xC0 | (cp >> 6)));
      out.push(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push(static_cast<char>(0xE0 | (cp >> 12)));
      out.push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push(static_cast<char>(0xF0 | (cp >> 18)));
      out.push(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };

  usize i = 0;
  while (i < body.length) {
    const char c = body[i];
    i++;

    if (c != '\\') {
      out.push(c);
      continue;
    }
    if (i >= body.length) {
      out.push('\\');
      break;
    }

    const char e = body[i];
    i++;
    switch (e) {
    case 'n': out.push('\n'); break;
    case 't': out.push('\t'); break;
    case 'r': out.push('\r'); break;
    case 'a': out.push('\a'); break;
    case 'b': out.push('\b'); break;
    case 'f': out.push('\f'); break;
    case 'v': out.push('\v'); break;
    case 'e':
    case 'E': out.push('\x1b'); break;
    case '\\': out.push('\\'); break;
    case '\'': out.push('\''); break;
    case '"': out.push('"'); break;
    case '?': out.push('?'); break;
    case 'x': {
      i32 value = 0;
      i32 digit_count = 0;
      while (digit_count < 2 && i < body.length) {
        let const digit = do_hex_value(body[i]);
        if (digit < 0) break;
        value = value * 16 + digit;
        i++;
        digit_count++;
      }
      if (digit_count == 0) {
        out.push('\\');
        out.push('x');
      } else {
        out.push(static_cast<char>(value));
      }
    } break;
    case 'c': {
      if (i >= body.length) {
        out.push('\\');
        out.push('c');
        break;
      }
      const char target = body[i];
      i++;
      if (target == '\\' && i < body.length && body[i] == '\\') {
        i++;
      }
      const char upper = (target >= 'a' && target <= 'z')
                             ? static_cast<char>(target - 'a' + 'A')
                             : target;
      const u8 control = upper == '?'
                             ? static_cast<u8>(0x7fu)
                             : static_cast<u8>(static_cast<u8>(upper) & 0x1fu);
      out.push(static_cast<char>(control));
    } break;
    case 'u':
    case 'U': {
      const i32 max_digit_count = e == 'u' ? 4 : 8;
      u32 codepoint = 0;
      i32 digit_count = 0;
      while (digit_count < max_digit_count && i < body.length) {
        let const digit = do_hex_value(body[i]);
        if (digit < 0) break;
        codepoint = codepoint * 16 + static_cast<u32>(digit);
        i++;
        digit_count++;
      }
      if (digit_count == 0) {
        out.push('\\');
        out.push(e);
      } else {
        do_emit_codepoint(codepoint);
      }
    } break;
    default:
      if (e >= '0' && e <= '7') {
        i32 value = e - '0';
        i32 digit_count = 1;
        while (digit_count < 3 && i < body.length && body[i] >= '0' &&
               body[i] <= '7')
        {
          value = value * 8 + (body[i] - '0');
          i++;
          digit_count++;
        }
        out.push(static_cast<char>(value));
      } else {
        out.push('\\');
        out.push(e);
      }
      break;
    }
  }
}

fn append_ansi_c_quote_if_needed(String &out, StringView arg) throws -> bool
{
  if (arg.is_empty()) {
    out += "''";
    return true;
  }

  bool has_control_byte = false;
  for (usize i = 0; i < arg.length; i++) {
    let const byte = static_cast<unsigned char>(arg[i]);
    if (byte < 0x20 || byte == 0x7f) {
      has_control_byte = true;
      break;
    }
  }
  if (!has_control_byte) return false;

  out += "$'";
  for (usize i = 0; i < arg.length; i++) {
    let const character = arg[i];
    switch (character) {
    case '\a': out += "\\a"; break;
    case '\b': out += "\\b"; break;
    case '\t': out += "\\t"; break;
    case '\n': out += "\\n"; break;
    case '\v': out += "\\v"; break;
    case '\f': out += "\\f"; break;
    case '\r': out += "\\r"; break;
    case '\x1b': out += "\\E"; break;
    case '\'': out += "\\'"; break;
    case '\\': out += "\\\\"; break;
    default: {
      let const byte = static_cast<unsigned char>(character);
      if (byte < 0x20 || byte == 0x7f) {
        out.push('\\');
        out.push(static_cast<char>('0' + ((byte >> 6) & 7)));
        out.push(static_cast<char>('0' + ((byte >> 3) & 7)));
        out.push(static_cast<char>('0' + (byte & 7)));
      } else {
        out.push(character);
      }
      break;
    }
    }
  }
  out += "'";
  return true;
}

/* Inspiration taken from https://github.com/tsoding/glob.h :3
 * This fragment is under MIT License (c) Alexey Kutepov <reximkut@gmail.com> */
static pure fn is_glob_char_active(const Bitset &glob_active,
                                   usize index) wontthrow -> bool
{
  return index < glob_active.count() && glob_active[index];
}

namespace {

/* One alternative of a bash extended-glob group, a slice of the glob and the
   mask offset that slice begins at. */
struct extglob_alternative
{
  StringView pattern;
  usize mask_offset;
};

hot fn extglob_active(const Bitset &mask, usize index) wontthrow -> bool
{
  return index < mask.count() ? mask[index] : true;
}

/* True when glob at index opens an extended-glob group, one of ?, *, +, @, or !
   immediately followed by (. The caller has opted into extglob, so the group
   structure is read from the text rather than the metacharacter mask, which
   only distinguishes a leaf star or bracket from a quoted literal. */
fn extglob_opens_group(StringView glob, usize index) wontthrow -> bool
{
  if (index + 1 >= glob.count()) return false;
  const char op = glob[index];
  if (op != '?' && op != '*' && op != '+' && op != '@' && op != '!') {
    return false;
  }
  return glob[index + 1] == '(';
}

/* The index of the ) that closes the group whose ( sits at glob[1], tracking
   nested groups by text. Returns glob.count() when the group is unbalanced. */
fn extglob_group_close(StringView glob) wontthrow -> usize
{
  usize depth = 0;
  for (usize i = 1; i < glob.count(); i++) {
    if (glob[i] == '(')
      depth++;
    else if (glob[i] == ')') {
      depth--;
      if (depth == 0) return i;
    }
  }
  return glob.count();
}

fn extglob_full_match(StringView glob, StringView str, const Bitset &mask,
                      usize mask_offset) throws -> bool;

/* Match min_reps or more repetitions of one of the alternatives against the
   front of str, then the suffix against the rest. The min drops to zero after
   the first repetition, so a + needs one and a * needs none. */
fn extglob_match_repetition(const ArrayList<extglob_alternative> &alternatives,
                            StringView suffix, usize suffix_offset,
                            StringView str, const Bitset &mask,
                            usize min_reps) throws -> bool
{
  if (min_reps == 0 && extglob_full_match(suffix, str, mask, suffix_offset)) {
    return true;
  }
  for (let const &alternative : alternatives) {
    for (usize length = 1; length <= str.count(); length++) {
      if (!extglob_full_match(alternative.pattern,
                              str.substring_of_length(0, length), mask,
                              alternative.mask_offset))
        continue;
      const usize next_min = min_reps > 0 ? min_reps - 1 : 0;
      if (extglob_match_repetition(alternatives, suffix, suffix_offset,
                                   str.substring(length), mask, next_min))
        return true;
    }
  }
  return false;
}

fn extglob_full_match(StringView glob, StringView str, const Bitset &mask,
                      usize mask_offset) throws -> bool
{
  if (glob.is_empty()) return str.is_empty();

  const bool active = extglob_active(mask, mask_offset);
  const char head = glob[0];

  /* An extended-glob group such as @(a|b), *(a|b), or !(a) drives the match
     through the alternatives split on the top-level |. */
  if (extglob_opens_group(glob, 0)) {
    const usize close = extglob_group_close(glob);
    if (close < glob.count()) {
      const StringView content = glob.substring_of_length(2, close - 2);
      const usize content_offset = mask_offset + 2;
      const StringView suffix = glob.substring(close + 1);
      const usize suffix_offset = mask_offset + close + 1;

      let alternatives = ArrayList<extglob_alternative>{heap_allocator()};
      usize depth = 0;
      usize start = 0;
      for (usize i = 0; i <= content.count(); i++) {
        const bool boundary =
            i == content.count() || (content[i] == '|' && depth == 0);
        if (boundary) {
          alternatives.push({content.substring_of_length(start, i - start),
                             content_offset + start});
          start = i + 1;
        } else if (content[i] == '(')
          depth++;
        else if (content[i] == ')')
          depth--;
      }

      switch (head) {
      case '*':
        return extglob_match_repetition(alternatives, suffix, suffix_offset,
                                        str, mask, 0);
      case '+':
        return extglob_match_repetition(alternatives, suffix, suffix_offset,
                                        str, mask, 1);
      case '?':
      case '@':
        for (let const &alternative : alternatives) {
          for (usize length = head == '?' ? 0 : 1; length <= str.count();
               length++)
          {
            if (extglob_full_match(alternative.pattern,
                                   str.substring_of_length(0, length), mask,
                                   alternative.mask_offset) &&
                extglob_full_match(suffix, str.substring(length), mask,
                                   suffix_offset))
              return true;
          }
        }
        /* A ? group also matches zero occurrences, so the suffix may follow
           with nothing consumed. */
        return head == '?' &&
               extglob_full_match(suffix, str, mask, suffix_offset);
      case '!':
        /* A negated group consumes a prefix that none of the alternatives
           match, then the suffix matches the rest. */
        for (usize length = 0; length <= str.count(); length++) {
          bool any_alternative_matches = false;
          for (let const &alternative : alternatives) {
            if (extglob_full_match(alternative.pattern,
                                   str.substring_of_length(0, length), mask,
                                   alternative.mask_offset))
            {
              any_alternative_matches = true;
              break;
            }
          }
          if (!any_alternative_matches &&
              extglob_full_match(suffix, str.substring(length), mask,
                                 suffix_offset))
          {
            return true;
          }
        }
        return false;
      default: break;
      }
    }
  }

  /* A trailing * matches the rest of the string, so it is taken without trying
     every split. */
  if (active && head == '*') {
    for (usize eaten = 0; eaten <= str.count(); eaten++) {
      if (extglob_full_match(glob.substring(1), str.substring(eaten), mask,
                             mask_offset + 1))
        return true;
    }
    return false;
  }

  if (str.is_empty()) return false;

  if (active && head == '?') {
    return extglob_full_match(glob.substring(1), str.substring(1), mask,
                              mask_offset + 1);
  }

  if (active && head == '[') {
    /* Reuse the iterative matcher for a single bracket class by matching one
       character, then continue with the rest of the glob and the string. */
    usize span = 1;
    while (span < glob.count() &&
           !(glob[span] == ']' && extglob_active(mask, mask_offset + span)))
      span++;
    if (span < glob.count()) {
      span++;
      const bool class_matched =
          glob_matches(glob.substring_of_length(0, span),
                       str.substring_of_length(0, 1), mask, mask_offset);
      if (!class_matched) return false;
      return extglob_full_match(glob.substring(span), str.substring(1), mask,
                                mask_offset + span);
    }
  }

  if (str[0] != head) return false;
  return extglob_full_match(glob.substring(1), str.substring(1), mask,
                            mask_offset + 1);
}

/* The POSIX character classes a bracket accepts as [:name:], each name bound
   to its ctype predicate through a packed-key map so the glob hot path pays
   a word compare rather than a name chain. The wrappers pin the byte through
   unsigned char, the only argument range the ctype functions define. */
using posix_class_test = bool (*)(u8 byte);

constexpr static_string_entry<posix_class_test> POSIX_CLASS_ENTRIES[] = {
    {SSK("alnum"),  [](u8 byte) { return std::isalnum(byte) != 0; } },
    {SSK("alpha"),  [](u8 byte) { return std::isalpha(byte) != 0; } },
    {SSK("blank"),  [](u8 byte) { return std::isblank(byte) != 0; } },
    {SSK("cntrl"),  [](u8 byte) { return std::iscntrl(byte) != 0; } },
    {SSK("digit"),  [](u8 byte) { return std::isdigit(byte) != 0; } },
    {SSK("graph"),  [](u8 byte) { return std::isgraph(byte) != 0; } },
    {SSK("lower"),  [](u8 byte) { return std::islower(byte) != 0; } },
    {SSK("print"),  [](u8 byte) { return std::isprint(byte) != 0; } },
    {SSK("punct"),  [](u8 byte) { return std::ispunct(byte) != 0; } },
    {SSK("space"),  [](u8 byte) { return std::isspace(byte) != 0; } },
    {SSK("upper"),  [](u8 byte) { return std::isupper(byte) != 0; } },
    {SSK("xdigit"), [](u8 byte) { return std::isxdigit(byte) != 0; }},
};

constexpr StaticStringMap POSIX_CLASSES{POSIX_CLASS_ENTRIES};

/* Whether the byte belongs to the named class. An unknown name matches
   nothing, the way bash treats a class it does not know. */
fn byte_is_in_posix_class(StringView class_name, u8 byte) throws -> bool
{
  if (const Maybe<posix_class_test> test = POSIX_CLASSES.find(class_name);
      test.has_value())
    return (*test)(byte);
  return false;
}

} /* namespace */

pure fn token_has_uppercase(StringView token) wontthrow -> bool
{
  for (usize position = 0; position < token.length; position++)
    if (token[position] >= 'A' && token[position] <= 'Z') return true;
  return false;
}

pure fn smart_case_prefix_matches(StringView candidate,
                                  StringView prefix) wontthrow -> bool
{
  if (candidate.starts_with(prefix)) return true;
  if (token_has_uppercase(prefix) || candidate.length < prefix.length)
    return false;

  for (usize position = 0; position < prefix.length; position++)
    if (ascii_to_lower(candidate[position]) != ascii_to_lower(prefix[position]))
      return false;

  return true;
}

fn glob_matches(StringView glob, StringView str, const Bitset &glob_active,
                usize mask_offset, bool extglob) throws -> bool
{
  /* The extended-glob grammar needs backtracking over alternatives and
     repetition, so it runs in a separate recursive matcher. It is taken only
     when extglob is on and the pattern actually holds a group, so a plain glob
     keeps the iterative matcher below, unchanged, and pays nothing. */
  if (extglob) {
    for (usize i = 0; i + 1 < glob.count(); i++) {
      const char c = glob[i];
      if ((c == '?' || c == '*' || c == '+' || c == '@' || c == '!') &&
          glob[i + 1] == '(')
      {
        return extglob_full_match(glob, str, glob_active, mask_offset);
      }
    }
  }

  usize s = 0;
  usize g = 0;
  usize star_glob_position = static_cast<usize>(-1);
  usize star_string_position = 0;

  while (s < str.count()) {
    if (g >= glob.count()) goto retry_star;
    ASSERT(g < glob.count() && s < str.count());

    if (!is_glob_char_active(glob_active, mask_offset + g)) {
      if (glob[g] != str[s]) goto retry_star;
      g++;
      s++;
      continue;
    }

    switch (glob[g]) {
    case '?': {
      g++;
      s++;
    } break;

    case '*': {
      while (g < glob.count() && glob[g] == '*' &&
             is_glob_char_active(glob_active, mask_offset + g))
        g++;
      if (g >= glob.count()) return true;
      star_glob_position = g;
      star_string_position = s;
    } break;

    case '[': {
      bool is_matched = false;
      bool should_negate = false;

      /* clang-format off */
#define GLOB_GROUP_ERR()                                                       \
  throw ErrorWithLocationAndDetails{                                           \
      {0, 0},                               \
      "Unclosed '[' group",                                                    \
      {0, 1},                                                \
      "expected ] here"                                                        \
  };
      /* clang-format on */

      /* A bracket member, a class terminator ], a negating ! or ^, and a range
         '-' carry their special meaning only when the byte is an active glob
         character. A quoted or escaped ] inside the class is a literal member,
         not the terminator, and a quoted member byte never opens a range, so
         the scan consults the same per-byte mask the rest of the matcher reads.
       */
      let const is_active = [&](usize index) wontthrow -> bool {
        return is_glob_char_active(glob_active, mask_offset + index);
      };
      let const is_close_at = [&](usize index) wontthrow -> bool {
        return glob[index] == ']' && is_active(index);
      };

      /* The unsigned value of a byte, so a high byte at or above 0x80 compares
         as itself rather than as a negative char in the range and equality
         tests. */
      let const byte_at = [](StringView view, usize index) wontthrow -> u8 {
        return static_cast<u8>(view[index]);
      };

      /* A [:name:] unit inside the bracket is a POSIX character class. The
         index past its closing ":]" comes back when one starts here, so both
         scans treat the unit atomically and its inner ] never closes the
         bracket. */
      let const class_end_past = [&](usize index) wontthrow -> Maybe<usize> {
        if (index + 1 >= glob.count() || glob[index] != '[' ||
            glob[index + 1] != ':' || !is_active(index))
          return None;
        for (usize scan = index + 2; scan + 1 < glob.count(); scan++) {
          if (glob[scan] == ':' && glob[scan + 1] == ']') return scan + 2;
          /* A ] before any ":]" means the [ was a plain member after all, the
             way [[:a] is a bracket holding [, :, and a. */
          if (glob[scan] == ']' && is_active(scan)) return None;
        }
        return None;
      };

      /* A bracket with no closing ] is not a character class, so the [ is a
         literal character, as POSIX specifies. A ] right after [ or [^ is a
         member, so the scan for the closing ] starts past it. */
      usize close_scan = g + 1;
      if (close_scan < glob.count() &&
          (glob[close_scan] == '!' || glob[close_scan] == '^') &&
          is_active(close_scan))
      {
        close_scan++;
      }
      if (close_scan < glob.count() && is_close_at(close_scan)) {
        close_scan++;
      }
      bool has_closing_bracket = false;
      while (close_scan < glob.count()) {
        if (Maybe<usize> past_class = class_end_past(close_scan);
            past_class.has_value())
        {
          close_scan = *past_class;
          continue;
        }
        if (is_close_at(close_scan)) {
          has_closing_bracket = true;
          break;
        }
        close_scan++;
      }
      if (!has_closing_bracket) {
        if (byte_at(glob, g) != byte_at(str, s)) goto retry_star;
        g++;
        s++;
        break;
      }

      g++;
      if (g >= glob.count()) GLOB_GROUP_ERR();

      /* POSIX sh negates a class with a leading '!'. The '^' form is kept as a
         common extension. The negation applies only to an active byte, so a
         quoted ! or ^ at the front is a literal member. */
      if ((glob[g] == '!' || glob[g] == '^') && is_active(g)) {
        g++;
        should_negate = true;

        if (g >= glob.count()) GLOB_GROUP_ERR();
      }

      /* The first member bypasses the close check, so a leading ] is a plain
         member. A range is consumed as one atom, so its first endpoint does not
         also match by itself and a later hyphen remains a literal member. */
      bool is_first_member = true;
      while (g < glob.count() && (is_first_member || !is_close_at(g))) {
        if (Maybe<usize> past_class = class_end_past(g); past_class.has_value())
        {
          let const class_name =
              glob.substring_of_length(g + 2, *past_class - g - 4);
          is_matched |= byte_is_in_posix_class(class_name, byte_at(str, s));
          g = *past_class;
          is_first_member = false;
          continue;
        }
        if (glob[g] != '-' && g + 2 < glob.count() && glob[g + 1] == '-' &&
            is_active(g + 1) && !is_close_at(g + 2) &&
            !class_end_past(g + 2).has_value())
        {
          let const lower = byte_at(glob, g);
          let const upper = byte_at(glob, g + 2);
          is_matched |= lower <= byte_at(str, s) && byte_at(str, s) <= upper;
          g += 3;
        } else {
          is_matched |= byte_at(glob, g) == byte_at(str, s);
          g++;
        }
        is_first_member = false;
      }

      if (g >= glob.count() || !is_close_at(g)) {
        GLOB_GROUP_ERR();
      }
      if (should_negate) is_matched = !is_matched;
      if (!is_matched) goto retry_star;

      g++;
      s++;
    } break;

    default:
      if (glob[g] != str[s]) goto retry_star;
      g++;
      s++;
    }
    continue;

retry_star:
    if (star_glob_position == static_cast<usize>(-1) ||
        star_string_position >= str.count())
      return false;
    star_string_position++;
    s = star_string_position;
    g = star_glob_position;
  }

  if (s >= str.count()) {
    while (g < glob.count() && glob[g] == '*' &&
           is_glob_char_active(glob_active, mask_offset + g))
    {
      g++;
    }

    if (g >= glob.count()) return true;
  }

  return false;
}

/* The one context quit reads the interactive state and the memory-report flag
   from, so quit gates the goodbye on a real interactive prompt and a script,
   a -c, or a subshell exits silently the way dash does. A null pointer, the
   state before the context exists, reads as a non-interactive shell with the
   report off. */
static const EvalContext *QUIT_CONTEXT = nullptr;

fn set_quit_context(const EvalContext *context) wontthrow -> void
{
  QUIT_CONTEXT = context;
}

/* The granular memory report, the live bump bytes and the reserved capacity of
   each arena, then the malloc heap in use. The arena capacity counts the blocks
   the bump allocator holds, while the heap figure counts the String buffers and
   other long-lived allocations the arenas do not own. */
cold fn print_memory_report() wontthrow -> void
{
  if (AST_ARENA != nullptr)
    std::fprintf(stderr, "AST arena: used %zu, reserved %zu, blocks %zu\n",
                 AST_ARENA->bytes_used(), AST_ARENA->bytes_capacity(),
                 AST_ARENA->block_count());
  if (FUNCTION_ARENA != nullptr)
    std::fprintf(stderr, "Function arena: used %zu, reserved %zu, blocks %zu\n",
                 FUNCTION_ARENA->bytes_used(), FUNCTION_ARENA->bytes_capacity(),
                 FUNCTION_ARENA->block_count());
  os::malloc_heap_stats heap_stats{};
  if (os::read_malloc_heap_stats(heap_stats))
    std::fprintf(stderr,
                 "Malloc heap: in use %zu, total arena %zu, mmapped %zu\n",
                 heap_stats.bytes_in_use, heap_stats.arena_bytes,
                 heap_stats.mapped_bytes);
}

[[noreturn]] fn quit(i32 code, farewell_policy farewell) throws -> void
{
  let const should_goodbye = farewell == farewell_policy::Goodbye;
  LOG(Info, "quitting with code %d", code);

  if (QUIT_CONTEXT != nullptr && QUIT_CONTEXT->memory_stats_enabled())
    print_memory_report();

  const u8 actual_code = static_cast<u8>(code);

  if (!os::is_child_process()) {
    if (toiletline::is_active()) {
      try {
        toiletline::exit();
      } catch (const Error &e) {
        show_message(e.to_string());
      }
    }

    if (should_goodbye && QUIT_CONTEXT != nullptr &&
        QUIT_CONTEXT->shell_is_interactive())
    {
      if (let const farewell =
              QUIT_CONTEXT->get_variable_value("KOSH_FAREWELL");
          farewell.has_value())
      {
        if (!farewell->is_empty()) {
          let message = String{heap_allocator(), farewell->view()};
          if (code != 0) {
            message += " (Code ";
            message += String::from(actual_code, heap_allocator());
            message += ')';
          }
          show_message(message);
        }
      } else {
        let message = String{heap_allocator(), "Goodbye :c"};
        if (code != 0) {
          message += " (Code ";
          message += String::from(actual_code, heap_allocator());
          message += ')';
        }
        show_message(message);
      }
    }
  }

  std::exit(actual_code);
}

struct cached_directory_listing
{
  u64 device_id{0};
  u64 file_id{0};
  i64 modification_time{0};
  u32 modification_nanoseconds{0};
  i64 change_time{0};
  u32 change_nanoseconds{0};
  u64 size{0};
  u64 generation{0};
  usize alias_count{0};
  bool has_file_identity{false};
  bool is_sorted{false};
  ArrayList<Path::directory_child> entries{heap_allocator()};
};

struct cached_directory_alias
{
  usize listing_position{0};
  u64 validation_epoch{0};
  u64 observed_generation{0};
};

static StringMap<cached_directory_alias> DIR_LISTING_ALIASES{heap_allocator()};
static StringMap<usize> DIR_LISTING_IDENTITIES{heap_allocator()};
static ArrayList<cached_directory_listing> DIR_LISTINGS{heap_allocator()};
static ArrayList<usize> FREE_DIR_LISTING_POSITIONS{heap_allocator()};
static u64 DIRECTORY_VALIDATION_EPOCH = 0;
static u64 DIRECTORY_LISTING_GENERATION = 0;
#if !defined NDEBUG
static usize DEBUG_DIRECTORY_STAT_COUNT = 0;
static usize DEBUG_DIRECTORY_READ_COUNT = 0;
static usize DEBUG_DIRECTORY_SORT_COUNT = 0;
static usize DEBUG_EXECUTABLE_PROBE_COUNT = 0;
static usize DEBUG_PROGRAM_PATH_CANDIDATE_COUNT = 0;
#endif

static fn clear_directory_listing_cache() throws -> void
{
  DIR_LISTING_ALIASES.clear();
  DIR_LISTING_IDENTITIES.clear();
  DIR_LISTINGS.clear();
  FREE_DIR_LISTING_POSITIONS.clear();
}

static fn begin_directory_validation_epoch() wontthrow -> void;

fn ProgramResolver::cache_resolved_path(StringView name, const Path &full_path,
                                        os::program_extension extension,
                                        bool is_bare_result) throws -> void
{
  let &entry = m_execution_cache.get_or_create(name, CacheEntry{});
  for (usize position = 0; position < entry.paths.count(); position++) {
    let &cached = entry.paths[position];
    if (cached.extension != extension) continue;
    cached.path = full_path;
    if (is_bare_result) entry.bare_path_position = position;
    return;
  }

  if (is_bare_result) {
    entry.bare_path_position = entry.paths.count();
  }
  entry.paths.push({full_path, extension});
}

pure fn ProgramResolver::find_cached_program_path(
    const CacheEntry &entry,
    os::program_extension wanted_extension) const wontthrow -> const Path *
{
  if (wanted_extension == os::program_extension::None) {
    if (!entry.bare_path_position.has_value()) return nullptr;
    return &entry.paths[*entry.bare_path_position].path;
  }

  for (let const &cached : entry.paths) {
    if (cached.extension != wanted_extension) continue;
    return &cached.path;
  }

  return nullptr;
}

static fn directory_identity_key(u64 device_id, u64 file_id) throws -> String
{
  let key = String{heap_allocator()};
  key.append(StringView{reinterpret_cast<const char *>(&device_id),
                        sizeof(device_id)});
  key.append(
      StringView{reinterpret_cast<const char *>(&file_id), sizeof(file_id)});
  return key;
}

static fn release_directory_listing_alias(usize position) throws -> void
{
  let &listing = DIR_LISTINGS[position];
  ASSERT(listing.alias_count > 0);
  listing.alias_count--;
  if (listing.alias_count != 0) return;

  if (listing.has_file_identity) {
    let const key = directory_identity_key(listing.device_id, listing.file_id);
    DIR_LISTING_IDENTITIES.erase(key.view());
  }
  listing = cached_directory_listing{};
  FREE_DIR_LISTING_POSITIONS.push(position);
}

static fn set_directory_listing_alias(StringView key, usize position) throws
    -> void
{
  let *existing = DIR_LISTING_ALIASES.find(key);
  if (existing != nullptr && existing->listing_position == position) {
    existing->validation_epoch = DIRECTORY_VALIDATION_EPOCH;
    existing->observed_generation = DIR_LISTINGS[position].generation;
    return;
  }

  if (existing != nullptr)
    release_directory_listing_alias(existing->listing_position);
  DIR_LISTINGS[position].alias_count++;
  DIR_LISTING_ALIASES.set(
      key, cached_directory_alias{position, DIRECTORY_VALIDATION_EPOCH,
                                  DIR_LISTINGS[position].generation});
}

static fn allocate_directory_listing_position() throws -> usize
{
  if (!FREE_DIR_LISTING_POSITIONS.is_empty()) {
    let const position = FREE_DIR_LISTING_POSITIONS.back();
    FREE_DIR_LISTING_POSITIONS.pop_back();
    return position;
  }

  let const position = DIR_LISTINGS.count();
  DIR_LISTINGS.push({});
  return position;
}

static pure fn directory_entry_folded_name_is_less(StringView left,
                                                   StringView right) wontthrow
    -> bool
{
  let const shared_length =
      left.length < right.length ? left.length : right.length;
  for (usize position = 0; position < shared_length; position++) {
    let const left_byte = ascii_to_lower(left[position]);
    let const right_byte = ascii_to_lower(right[position]);
    if (left_byte != right_byte) return left_byte < right_byte;
  }
  if (left.length != right.length) return left.length < right.length;

  return false;
}

static pure fn directory_entry_name_is_less(StringView left,
                                            StringView right) wontthrow -> bool
{
  if (directory_entry_folded_name_is_less(left, right)) return true;
  if (directory_entry_folded_name_is_less(right, left)) return false;

  return left < right;
}

pure fn directory_entry_name_lower_bound(
    const ArrayList<Path::directory_child> &entries, StringView name) wontthrow
    -> usize
{
  usize lower = 0;
  usize upper = entries.count();
  while (lower < upper) {
    let const middle = lower + (upper - lower) / 2;
    if (directory_entry_folded_name_is_less(entries[middle].name.view(), name))
      lower = middle + 1;
    else
      upper = middle;
  }

  return lower;
}

pure fn directory_entry_name_has_casefold_prefix(StringView name,
                                                 StringView prefix) wontthrow
    -> bool
{
  if (name.length < prefix.length) return false;

  for (usize position = 0; position < prefix.length; position++)
    if (ascii_to_lower(name[position]) != ascii_to_lower(prefix[position]))
      return false;

  return true;
}

fn read_directory_cached(const Path &directory, directory_validation validation,
                         directory_listing_order order) throws
    -> const ArrayList<Path::directory_child> *
{
  let const do_apply_order =
      [&](cached_directory_listing &listing)
          throws -> const ArrayList<Path::directory_child> * {
    if (order == directory_listing_order::FoldedName && !listing.is_sorted) {
#if !defined NDEBUG
      DEBUG_DIRECTORY_SORT_COUNT++;
#endif
      listing.entries.sort([](const Path::directory_child &left,
                              const Path::directory_child &right) {
        return directory_entry_name_is_less(left.name.view(),
                                            right.name.view());
      });
      listing.is_sorted = true;
    }

    return &listing.entries;
  };

  let const key = directory.text().view();
  let *alias = DIR_LISTING_ALIASES.find(key);
  if (validation == directory_validation::Cached && alias != nullptr &&
      alias->validation_epoch == DIRECTORY_VALIDATION_EPOCH &&
      alias->observed_generation ==
          DIR_LISTINGS[alias->listing_position].generation)
  {
    return do_apply_order(DIR_LISTINGS[alias->listing_position]);
  }

#if !defined NDEBUG
  DEBUG_DIRECTORY_STAT_COUNT++;
#endif
  os::file_status status{};
  let const has_status = os::stat_path_following(key, status);
  let physical_position = Maybe<usize>{};
  if (has_status && status.has_file_identity) {
    let const identity_key =
        directory_identity_key(status.device_id, status.file_id);
    if (let const *position = DIR_LISTING_IDENTITIES.find(identity_key.view()))
      physical_position = *position;
  }

  if (physical_position.has_value()) {
    let &cached = DIR_LISTINGS[*physical_position];
    if (cached.modification_time == status.modification_time &&
        cached.modification_nanoseconds == status.modification_nanoseconds &&
        cached.change_time == status.change_time &&
        cached.change_nanoseconds == status.change_nanoseconds &&
        cached.size == status.size)
    {
      set_directory_listing_alias(key, *physical_position);
      return do_apply_order(cached);
    }
  }

#if !defined NDEBUG
  DEBUG_DIRECTORY_READ_COUNT++;
#endif
  let entries = Path::read_directory_typed(directory);
  if (!entries.has_value()) return nullptr;

  for (let &child : *entries) {
    if (child.kind != Path::entry_kind::Unknown) continue;

    let full_path = directory.clone();
    full_path.push_component(child.name.view());
    if (full_path.is_directory())
      child.kind = Path::entry_kind::Directory;
    else if (full_path.is_regular_file())
      child.kind = Path::entry_kind::Regular;
    else
      child.kind = Path::entry_kind::Other;
  }

  cached_directory_listing fresh{};
  fresh.device_id = has_status ? status.device_id : 0;
  fresh.file_id = has_status ? status.file_id : 0;
  fresh.modification_time = has_status ? status.modification_time : 0;
  fresh.modification_nanoseconds =
      has_status ? status.modification_nanoseconds : 0;
  fresh.change_time = has_status ? status.change_time : 0;
  fresh.change_nanoseconds = has_status ? status.change_nanoseconds : 0;
  fresh.size = has_status ? status.size : 0;
  fresh.generation = ++DIRECTORY_LISTING_GENERATION;
  fresh.has_file_identity = has_status && status.has_file_identity;
  fresh.entries = steal(*entries);
  if (physical_position.has_value()) {
    fresh.alias_count = DIR_LISTINGS[*physical_position].alias_count;
    DIR_LISTINGS[*physical_position] = steal(fresh);
  } else {
    physical_position = allocate_directory_listing_position();
    DIR_LISTINGS[*physical_position] = steal(fresh);
    if (DIR_LISTINGS[*physical_position].has_file_identity) {
      let const identity_key =
          directory_identity_key(DIR_LISTINGS[*physical_position].device_id,
                                 DIR_LISTINGS[*physical_position].file_id);
      DIR_LISTING_IDENTITIES.set(identity_key.view(), *physical_position);
    }
  }
  set_directory_listing_alias(key, *physical_position);

  return do_apply_order(DIR_LISTINGS[*physical_position]);
}

fn directory_entry_kind(const Path &directory,
                        const Path::directory_child &entry) throws
    -> Path::entry_kind
{
  if (entry.kind != Path::entry_kind::Symlink) return entry.kind;

  let full_path = directory.clone();
  full_path.push_component(entry.name.view());
  if (full_path.is_directory()) return Path::entry_kind::Directory;
  if (full_path.is_regular_file()) return Path::entry_kind::Regular;
  return Path::entry_kind::Other;
}

pure fn directory_listing_generation(const Path &directory) wontthrow -> u64
{
  let const *alias = DIR_LISTING_ALIASES.find(directory.text().view());
  if (alias == nullptr) return 0;
  return DIR_LISTINGS[alias->listing_position].generation;
}

static fn sort_and_deduplicate_names(ArrayList<String> &names) throws -> void
{
  let positions = ArrayList<usize>{names.allocator()};
  positions.reserve(names.count());
  for (usize position = 0; position < names.count(); position++)
    positions.push(position);
  positions.sort([&](usize left, usize right) {
    return names[left].view() < names[right].view();
  });

  let sorted_names = ArrayList<String>{names.allocator()};
  sorted_names.reserve(names.count());
  for (let const position : positions)
    if (sorted_names.is_empty() ||
        sorted_names.back().view() != names[position].view())
      sorted_names.push(steal(names[position]));
  names = steal(sorted_names);
}

static fn begin_directory_validation_epoch() wontthrow -> void
{
  DIRECTORY_VALIDATION_EPOCH++;
}

ProgramResolver::ProgramResolver()
    : m_path(os::get_environment_variable("PATH"))
{}

ProgramResolver::ProgramResolver(Maybe<String> path) : m_path(steal(path)) {}

fn ProgramResolver::mark_command_name_indexes_stale() wontthrow -> void
{
  m_validated_prefix.clear();
  m_command_names_are_valid = false;
  m_command_names_validation_epoch = 0;
  m_prefix_validation_epoch = 0;
}

fn ProgramResolver::clear_command_name_indexes() wontthrow -> void
{
  m_command_names.clear();
  m_regular_names.clear();
  mark_command_name_indexes_stale();
}

fn ProgramResolver::mark_derived_indexes_stale() wontthrow -> void
{
  mark_command_name_indexes_stale();
  m_path_directory_generations.clear();
  m_path_directory_generations_are_valid = false;
  m_path_directories_validation_epoch = 0;
}

fn ProgramResolver::clear_derived_indexes() wontthrow -> void
{
  m_command_names.clear();
  m_regular_names.clear();
  mark_derived_indexes_stale();
}

fn ProgramResolver::assign_path(Maybe<String> path) throws -> void
{
  if (m_path.has_value() == path.has_value() &&
      (!m_path.has_value() || m_path->view() == path->view()))
  {
    m_execution_cache.clear();
    return;
  }

  let path_dirs = path.has_value() ? split_path_dirs(path->view())
                                   : ArrayList<String>{heap_allocator()};
  let index_path_dirs = deduplicate_path_dirs(path_dirs);
  let const path_search_changed = get_index_path_dirs() != index_path_dirs;
  m_path = steal(path);
  m_path_dirs = steal(path_dirs);
  m_index_path_dirs = steal(index_path_dirs);
  m_path_dirs_are_valid = true;
  m_execution_cache.clear();
  if (!path_search_changed) return;

  clear_derived_indexes();
}

fn ProgramResolver::restore_path(Maybe<String> path) throws -> void
{
  assign_path(steal(path));
}

fn ProgramResolver::invalidate() throws -> void
{
  m_execution_cache.clear();
  clear_derived_indexes();
  clear_directory_listing_cache();
}

fn ProgramResolver::remember_path(StringView name, const Path &path) throws
    -> void
{
  cache_resolved_path(name, path, os::program_extension::None, true);
}

fn ProgramResolver::split_path_dirs(StringView path) throws -> ArrayList<String>
{
  let directories = ArrayList<String>{heap_allocator()};
  let current = String{heap_allocator()};

  for (usize position = 0; position < path.length; position++) {
    const char byte = path[position];
    if (byte == os::PATH_DELIMITER) {
      directories.push(current.is_empty() ? String{"."}
                                          : String{current.view()});
      current.clear();
    } else {
      current.push(byte);
    }
  }
  directories.push(current.is_empty() ? String{"."} : String{current.view()});

  return directories;
}

fn ProgramResolver::deduplicate_path_dirs(
    const ArrayList<String> &directories) throws -> ArrayList<String>
{
  let unique_directories = ArrayList<String>{heap_allocator()};
  for (let const &directory : directories)
    if (!unique_directories.find(directory.view()).has_value())
      unique_directories.push(String{directory.view()});

  return unique_directories;
}

fn ProgramResolver::get_path_dirs() throws -> const ArrayList<String> &
{
  if (m_path_dirs_are_valid) return m_path_dirs;

  m_path_dirs = m_path.has_value() ? split_path_dirs(m_path->view())
                                   : ArrayList<String>{heap_allocator()};
  m_index_path_dirs = deduplicate_path_dirs(m_path_dirs);
  m_path_dirs_are_valid = true;

  return m_path_dirs;
}

fn ProgramResolver::get_index_path_dirs() throws -> const ArrayList<String> &
{
  unused(get_path_dirs());

  return m_index_path_dirs;
}

fn ProgramResolver::working_directory_changed() throws -> void
{
  begin_directory_validation_epoch();

  for (let const &directory : get_index_path_dirs())
    if (!Path{directory.view()}.is_absolute()) {
      m_execution_cache.clear();
      mark_derived_indexes_stale();
      return;
    }
}

fn ProgramResolver::refresh_path_directory_generations() throws -> void
{
  m_path_directory_generations.clear();
  for (let const &directory_text : get_index_path_dirs()) {
    let const directory = Path{directory_text.view()};
    let const entries =
        read_directory_cached(directory, directory_validation::Validate);
    m_path_directory_generations.push(
        entries == nullptr ? 0 : directory_listing_generation(directory));
  }
  m_path_directory_generations_are_valid = true;
  m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
}

fn ProgramResolver::rebuild_path_command_index(CompletionRefresh refresh) throws
    -> void
{
  if (refresh == CompletionRefresh::Fresh) {
    clear_derived_indexes();
  } else {
    ASSERT(m_path_directory_generations_are_valid);
    clear_command_name_indexes();
  }
  if (!m_path.has_value()) {
    m_command_names_are_valid = true;
    m_command_names_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
    m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
    m_prefix_validation_epoch = 0;
    m_validated_prefix.clear();
    return;
  }

  if (refresh == CompletionRefresh::Fresh) refresh_path_directory_generations();

  for (let const &directory_text : get_index_path_dirs()) {
    let const directory = Path{directory_text.view()};
    let const entries =
        read_directory_cached(directory, directory_validation::Cached);
    if (entries == nullptr) continue;

    for (let const &entry : *entries) {
      let full_path = directory.clone();
      full_path.push_component(entry.name.view());
      if (entry.kind == Path::entry_kind::Symlink && !full_path.exists())
        continue;
      if (directory_entry_kind(directory, entry) != Path::entry_kind::Regular)
        continue;

      let normalized_name = entry.name.clone();
      let const name_info = os::normalize_program_name(normalized_name);
      let const stem =
          normalized_name.substring_of_length(0, name_info.stem_length);
      m_regular_names.push(String{normalized_name.view()});
      if (stem.length != normalized_name.length())
        m_regular_names.push(String{stem});

#if !defined NDEBUG
      DEBUG_EXECUTABLE_PROBE_COUNT++;
#endif
      if (!full_path.is_executable()) continue;
      if (stem.length != entry.name.length())
        m_command_names.push(String{stem});
      m_command_names.push(steal(normalized_name));
    }
  }

  sort_and_deduplicate_names(m_command_names);
  sort_and_deduplicate_names(m_regular_names);
  m_command_names_are_valid = true;
  m_command_names_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
  m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
  m_prefix_validation_epoch = 0;
  m_validated_prefix.clear();
}

fn ProgramResolver::initialize_path_map() throws -> void
{
  LOG(Info, "scanning %zu unique PATH directories to seed the program cache",
      get_index_path_dirs().count());
  rebuild_path_command_index(CompletionRefresh::Fresh);
}

fn ProgramResolver::begin_explicit_completion(CompletionRefresh refresh) throws
    -> void
{
  if (m_explicit_completion_depth == 0) {
    switch (refresh) {
    case CompletionRefresh::Cached:
      m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
      m_command_names_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
      m_prefix_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
      break;
    case CompletionRefresh::Fresh: begin_directory_validation_epoch(); break;
    }
  }
  m_explicit_completion_depth++;
}

fn ProgramResolver::end_explicit_completion() wontthrow -> void
{
  ASSERT(m_explicit_completion_depth > 0);
  m_explicit_completion_depth--;
}

#if !defined NDEBUG
pure fn debug_directory_stat_count() wontthrow -> usize
{
  return DEBUG_DIRECTORY_STAT_COUNT;
}

pure fn debug_directory_read_count() wontthrow -> usize
{
  return DEBUG_DIRECTORY_READ_COUNT;
}

pure fn debug_directory_sort_count() wontthrow -> usize
{
  return DEBUG_DIRECTORY_SORT_COUNT;
}

pure fn debug_executable_probe_count() wontthrow -> usize
{
  return DEBUG_EXECUTABLE_PROBE_COUNT;
}

pure fn debug_program_path_candidate_count() wontthrow -> usize
{
  return DEBUG_PROGRAM_PATH_CANDIDATE_COUNT;
}
#endif

fn ProgramResolver::validate_path_directory_generations() throws -> bool
{
  if (m_path_directory_generations_are_valid &&
      m_path_directories_validation_epoch == DIRECTORY_VALIDATION_EPOCH)
    return false;

  bool did_change =
      !m_path_directory_generations_are_valid ||
      m_path_directory_generations.count() != get_index_path_dirs().count();
  let observed_generations = ArrayList<u64>{heap_allocator()};
  observed_generations.reserve(get_index_path_dirs().count());
  usize directory_position = 0;
  for (let const &directory_text : get_index_path_dirs()) {
    let const directory = Path{directory_text.view()};
    let const entries =
        read_directory_cached(directory, directory_validation::Validate);
    let const generation =
        entries == nullptr ? 0 : directory_listing_generation(directory);
    if (directory_position >= m_path_directory_generations.count() ||
        m_path_directory_generations[directory_position] != generation)
    {
      did_change = true;
    }
    observed_generations.push(generation);
    directory_position++;
  }
  m_path_directory_generations = steal(observed_generations);
  m_path_directory_generations_are_valid = true;
  m_path_directories_validation_epoch = DIRECTORY_VALIDATION_EPOCH;

  return did_change;
}

fn ProgramResolver::revalidate_command_prefix(StringView prefix) throws -> void
{
  clear_command_name_indexes();

  for (let const &directory_text : get_index_path_dirs()) {
    let const directory = Path{directory_text.view()};
    let const entries =
        read_directory_cached(directory, directory_validation::Cached);
    if (entries == nullptr) continue;

    for (let const &entry : *entries) {
      let normalized_name = entry.name.clone();
      let const name_info = os::normalize_program_name(normalized_name);
      let const stem =
          normalized_name.substring_of_length(0, name_info.stem_length);
      let const full_name_matches =
          smart_case_prefix_matches(normalized_name.view(), prefix);
      let const stem_matches = stem.length != normalized_name.length() &&
                               smart_case_prefix_matches(stem, prefix);
      if (!full_name_matches && !stem_matches) continue;

      let full_path = directory.clone();
      full_path.push_component(entry.name.view());
      if (entry.kind == Path::entry_kind::Symlink && !full_path.exists())
        continue;
      if (directory_entry_kind(directory, entry) != Path::entry_kind::Regular)
        continue;

      if (stem_matches) m_regular_names.push(String{stem});
      if (full_name_matches)
        m_regular_names.push(String{normalized_name.view()});

#if !defined NDEBUG
      DEBUG_EXECUTABLE_PROBE_COUNT++;
#endif
      if (!full_path.is_executable()) continue;
      if (stem_matches) m_command_names.push(String{stem});
      if (full_name_matches) m_command_names.push(steal(normalized_name));
    }
  }

  sort_and_deduplicate_names(m_command_names);
  sort_and_deduplicate_names(m_regular_names);
  m_validated_prefix = String{prefix};
  m_prefix_validation_epoch = DIRECTORY_VALIDATION_EPOCH;
}

fn ProgramResolver::prepare_complete_path_cache(
    StringView validation_prefix, ValidationScope validation_scope) throws
    -> void
{
  if (!m_command_names_are_valid && !m_path_directory_generations_are_valid &&
      m_explicit_completion_depth == 0)
    return;

  if (!m_command_names_are_valid) {
    if (validation_scope == ValidationScope::All ||
        validation_prefix.is_empty())
    {
      if (m_path_directory_generations_are_valid &&
          m_path_directories_validation_epoch == DIRECTORY_VALIDATION_EPOCH)
        rebuild_path_command_index(CompletionRefresh::Cached);
      else
        initialize_path_map();
      return;
    }

    if (!m_path_directory_generations_are_valid) {
      refresh_path_directory_generations();
      m_execution_cache.clear();
    } else if (m_path_directories_validation_epoch !=
               DIRECTORY_VALIDATION_EPOCH)
    {
      if (validate_path_directory_generations()) {
        m_execution_cache.clear();
        clear_command_name_indexes();
      }
    }

    if (!m_validated_prefix.is_empty() &&
        m_prefix_validation_epoch == DIRECTORY_VALIDATION_EPOCH &&
        validation_prefix.starts_with(m_validated_prefix.view()))
      return;

    revalidate_command_prefix(validation_prefix);
    return;
  }
  if (m_explicit_completion_depth == 0) return;
  if (m_command_names_validation_epoch == DIRECTORY_VALIDATION_EPOCH) return;

  if (validate_path_directory_generations()) m_execution_cache.clear();
  if (validation_scope == ValidationScope::Prefix &&
      !validation_prefix.is_empty())
  {
    m_command_names_are_valid = false;
    revalidate_command_prefix(validation_prefix);
    return;
  }
  rebuild_path_command_index(CompletionRefresh::Cached);
}

fn ProgramResolver::get_command_names(StringView validation_prefix,
                                      ValidationScope validation_scope) throws
    -> const ArrayList<String> &
{
  prepare_complete_path_cache(validation_prefix, validation_scope);

  return m_command_names;
}

pure fn ProgramResolver::command_name_lower_bound_in(
    const ArrayList<String> &names, StringView name) const wontthrow -> usize
{
  usize lower = 0;
  usize upper = names.count();
  while (lower < upper) {
    let const middle = lower + (upper - lower) / 2;
    if (names[middle].view() < name)
      lower = middle + 1;
    else
      upper = middle;
  }

  return lower;
}

pure fn ProgramResolver::get_command_name_lower_bound(
    StringView name) const wontthrow -> usize
{
  return command_name_lower_bound_in(m_command_names, name);
}

fn ProgramResolver::command_name_has_prefix(StringView prefix) throws -> bool
{
  let normalized_prefix = String{prefix};
  unused(os::normalize_program_name(normalized_prefix));
  prepare_complete_path_cache(normalized_prefix.view(),
                              ValidationScope::Prefix);
  for (let const &name : m_command_names)
    if (smart_case_prefix_matches(name.view(), normalized_prefix.view()))
      return true;

  return false;
}

pure fn ProgramResolver::has_valid_command_names() const wontthrow -> bool
{
  return m_command_names_are_valid;
}

fn ProgramResolver::get_status(StringView name, StatusLookup lookup) throws
    -> Status
{
  if (lookup == StatusLookup::Authoritative) {
    let const paths = search(name, SearchMode::First, Requirement::Execution,
                             CachePolicy::Bypass);
    if (paths.is_empty()) return Status::Missing;
    if (paths[0].is_executable()) return Status::Runnable;
    return Status::Blocked;
  }

  let normalized_name = String{name};
  unused(os::normalize_program_name(normalized_name));
  if (!m_command_names_are_valid && !normalized_name.is_empty())
    prepare_complete_path_cache(normalized_name.substring_of_length(0, 1),
                                ValidationScope::Prefix);

  let const runnable_position =
      command_name_lower_bound_in(m_command_names, normalized_name.view());
  if (runnable_position < m_command_names.count() &&
      m_command_names[runnable_position].view() == normalized_name.view())
    return Status::Runnable;
  let const regular_position =
      command_name_lower_bound_in(m_regular_names, normalized_name.view());
  if (regular_position < m_regular_names.count() &&
      m_regular_names[regular_position].view() == normalized_name.view())
    return Status::Blocked;

  return Status::Missing;
}

fn ProgramResolver::resolve_along_path(StringView program_name,
                                       SearchMode search_mode,
                                       Requirement requirement,
                                       CachePolicy cache_policy,
                                       Maybe<StringView> path_override) throws
    -> ArrayList<Path>
{
  if (!path_override.has_value() && !m_path.has_value())
    return ArrayList<Path>{heap_allocator()};

  LOG(Debug, "statting candidates for '%.*s' along PATH%s",
      static_cast<int>(program_name.length), program_name.data,
      search_mode == SearchMode::All ? ", collecting every match" : "");

  let result = ArrayList<Path>{heap_allocator()};

  let normalized_name = String{program_name};
  let const name_info = os::normalize_program_name(normalized_name);
  let const key = normalized_name.substring_of_length(0, name_info.stem_length);
  let blocked = Maybe<CachedPath>{};
  let override_directories = ArrayList<String>{heap_allocator()};
  const ArrayList<String> *directories;
  if (path_override.has_value()) {
    override_directories = split_path_dirs(*path_override);
    directories = &override_directories;
  } else {
    directories = &get_path_dirs();
  }

  for (let const &dir_string : *directories) {
    let const directory = Path{dir_string.view()};

    let full_path = directory.clone();
    full_path.push_component(program_name);

    if (name_info.extension == os::program_extension::None) {
      for (let const &suffix : os::PROGRAM_SUFFIXES) {
        let const try_path = Path{(full_path.text() + suffix.text).view()};

#if !defined NDEBUG
        DEBUG_PROGRAM_PATH_CANDIDATE_COUNT++;
#endif
        if (!try_path.is_regular_file()) continue;
        let const is_runnable = try_path.is_executable();
        let const is_match = requirement == Requirement::Regular || is_runnable;
        if (search_mode == SearchMode::All) {
          if (is_match) result.push(try_path);
          continue;
        }
        if (is_match) {
          result.push(try_path);
          if (cache_policy == CachePolicy::Remember && is_runnable)
            cache_resolved_path(key, try_path, suffix.extension, true);
          return result;
        }
        if (requirement == Requirement::Execution && !blocked.has_value())
          blocked = CachedPath{try_path, suffix.extension};
      }
    } else {
#if !defined NDEBUG
      DEBUG_PROGRAM_PATH_CANDIDATE_COUNT++;
#endif
      if (!full_path.is_regular_file()) continue;
      let const is_runnable = full_path.is_executable();
      let const is_match = requirement == Requirement::Regular || is_runnable;
      if (search_mode == SearchMode::All) {
        if (is_match) result.push(full_path);
      } else if (is_match) {
        result.push(full_path);
        if (cache_policy == CachePolicy::Remember && is_runnable)
          cache_resolved_path(key, full_path, name_info.extension, false);
        return result;
      } else if (requirement == Requirement::Execution && !blocked.has_value())
        blocked = CachedPath{full_path, name_info.extension};
    }
  }

  if (search_mode == SearchMode::First && blocked.has_value()) {
    result.push(blocked->path);
  }

  return result;
}

hot fn ProgramResolver::search(StringView program_name, SearchMode search_mode,
                               Requirement requirement,
                               CachePolicy cache_policy,
                               Maybe<StringView> path_override) throws
    -> ArrayList<Path>
{
  if (os::has_directory_separator(program_name)) {
    let result = ArrayList<Path>{heap_allocator()};
    let const candidate = Path{program_name};
    if (!candidate.is_regular_file()) return result;
    if (requirement != Requirement::Regular && !candidate.is_executable())
      return result;
    result.push(candidate);
    return result;
  }

  if (search_mode == SearchMode::All || cache_policy == CachePolicy::Bypass ||
      path_override.has_value())
    return resolve_along_path(program_name, search_mode, requirement,
                              CachePolicy::Bypass, path_override);

  let normalized_name = String{program_name};
  let const name_info = os::normalize_program_name(normalized_name);
  let const stem =
      normalized_name.substring_of_length(0, name_info.stem_length);

  if (const CacheEntry *const cached = m_execution_cache.find(stem);
      cached != nullptr)
  {
    let result = ArrayList<Path>{heap_allocator()};
    let const path = find_cached_program_path(*cached, name_info.extension);
    if (path != nullptr) {
      if (!path->is_regular_file() || !path->is_executable()) {
        m_execution_cache.erase(stem);
        return resolve_along_path(program_name, SearchMode::First, requirement,
                                  cache_policy, path_override);
      }
      result.push(*path);
      return result;
    }
  }

  return resolve_along_path(program_name, SearchMode::First, requirement,
                            cache_policy, path_override);
}

/* The rolling distance rows are fixed-width stack arrays, so a candidate name
   longer than this is treated as too far rather than indexed past the row. */
constexpr usize OSA_ROW_WIDTH = 256;

/* The optimal-string-alignment distance, the edit distance that also counts an
   adjacent transposition as one edit, so a typo such as gti for git scores one
   rather than two. Bounded by max_distance, returning max_distance + 1 once the
   best possible result on the current row already exceeds it, so a far-off
   candidate costs little. */
pure fn bounded_osa_distance(StringView a, StringView b,
                             usize max_distance) wontthrow -> usize
{
  const usize a_length = a.length;
  const usize b_length = b.length;
  if (a_length > b_length ? a_length - b_length > max_distance
                          : b_length - a_length > max_distance)
    return max_distance + 1;
  if (a_length == 0) return b_length;
  if (b_length == 0) return a_length;
  /* The rolling rows are indexed up to b_length, so a candidate longer than the
     row width is rejected before the rows are reserved. */
  if (b_length + 1 > OSA_ROW_WIDTH) return max_distance + 1;

  usize previous_previous[OSA_ROW_WIDTH];
  usize previous[OSA_ROW_WIDTH];
  usize current[OSA_ROW_WIDTH];

  for (usize j = 0; j <= b_length; j++)
    previous[j] = j;
  for (usize i = 1; i <= a_length; i++) {
    current[0] = i;
    usize row_best = current[0];
    for (usize j = 1; j <= b_length; j++) {
      const usize cost = a[i - 1] == b[j - 1] ? 0 : 1;
      usize value = previous[j] + 1;
      if (current[j - 1] + 1 < value) value = current[j - 1] + 1;
      if (previous[j - 1] + cost < value) value = previous[j - 1] + cost;
      if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1] &&
          previous_previous[j - 2] + 1 < value)
      {
        value = previous_previous[j - 2] + 1;
      }
      current[j] = value;
      if (value < row_best) row_best = value;
    }
    if (row_best > max_distance) return max_distance + 1;
    for (usize j = 0; j <= b_length; j++) {
      previous_previous[j] = previous[j];
      previous[j] = current[j];
    }
  }
  return previous[b_length];
}

pure fn suggestion_distance_budget(usize name_length) wontthrow -> usize
{
  return name_length <= 3 ? 1 : 2;
}

fn suggest_command(StringView name, const ArrayList<String> &local_names,
                   const ProgramResolver *resolver) throws -> Maybe<String>
{
  if (name.is_empty()) return None;

  let suggestion = NameSuggestion{name};

  for (let const &local : local_names)
    suggestion.consider(local.view());
  for (let const &builtin : builtin_names())
    suggestion.consider(builtin.view());
  if (resolver != nullptr && resolver->has_valid_command_names())
    resolver->for_each_command_name(
        [&](const String &entry) { suggestion.consider(entry.view()); });

  return suggestion.take_suggestion();
}

fn suggest_directory_entry(const Path &directory, StringView name) throws
    -> Maybe<String>
{
  if (name.is_empty()) return None;

  let const entries = read_directory_cached(directory);
  if (entries == nullptr) return None;

  let directory_names = ArrayList<StringView>{heap_allocator()};
  for (let const &entry : *entries)
    if (directory_entry_kind(directory, entry) == Path::entry_kind::Directory)
      directory_names.push(entry.name.view());
  directory_names.sort();

  let suggestion = NameSuggestion{name};
  for (let const directory_name : directory_names)
    suggestion.consider(directory_name);
  return suggestion.take_suggestion();
}

fn read_entire_standard_input() throws -> String
{
  let contents = os::read_fd_to_string(KOSH_STDIN, heap_allocator());
  if (!contents.has_value())
    throw Error{"Unable to read standard input: " +
                os::last_system_error_message()};
  return steal(*contents);
}

fn read_line_from_fd(os::descriptor fd, bool &was_delimiter_terminated,
                     char delimiter, u64 deadline_nanos, bool *was_timed_out,
                     Allocator allocator, bool *did_read_fail) throws
    -> Maybe<String>
{
  if (did_read_fail != nullptr) *did_read_fail = false;
  let line = String{allocator};
  bool has_read_any_byte = false;
  loop
  {
    if (deadline_nanos != 0) {
      let const now_nanos = os::monotonic_nanos();
      if (now_nanos >= deadline_nanos) {
        if (was_timed_out != nullptr) *was_timed_out = true;
        break;
      }
      let const remaining_nanos_unsigned = deadline_nanos - now_nanos;
      let const remaining_nanos = static_cast<i64>(
          remaining_nanos_unsigned > static_cast<u64>(INT64_MAX)
              ? INT64_MAX
              : remaining_nanos_unsigned);
      let const readable = os::wait_for_fd_readable(fd, remaining_nanos);
      if (readable != 1) {
        if (readable == 0 && was_timed_out != nullptr) *was_timed_out = true;
        break;
      }
    }

    u8 one_byte = 0;
    Maybe<usize> read_count = os::read_fd(fd, &one_byte, 1);
    if (!read_count.has_value()) {
      if (did_read_fail != nullptr) *did_read_fail = true;
      break;
    }
    if (*read_count == 0) break;
    has_read_any_byte = true;
    if (one_byte == static_cast<u8>(delimiter)) {
      was_delimiter_terminated = true;
      return line;
    }
    line.push(one_byte);
  }

  /* The loop fell out at end of input, so no delimiter ended the line. The read
     builtin maps an unterminated final line to a non-zero status while still
     assigning the bytes it read, the way dash does. */
  was_delimiter_terminated = false;

  if (!has_read_any_byte) return None;

  return line;
}

BufferedLineReader::BufferedLineReader(os::descriptor descriptor)
    : m_descriptor(descriptor)
{}

fn BufferedLineReader::next() throws -> Result
{
  m_line.clear();

  loop
  {
    usize delimiter_position = m_buffer_position;
    while (delimiter_position < m_buffer_length &&
           m_buffer[delimiter_position] != '\n')
    {
      delimiter_position++;
    }

    m_line.append(StringView{m_buffer + m_buffer_position,
                             delimiter_position - m_buffer_position});
    m_buffer_position = delimiter_position;
    if (m_buffer_position < m_buffer_length) {
      m_buffer_position++;
      return Result::Line;
    }
    if (m_is_at_end) return m_line.is_empty() ? Result::End : Result::Line;

    let const read_size = os::read_fd(m_descriptor, m_buffer, sizeof(m_buffer));
    if (!read_size.has_value()) return Result::Error;

    m_buffer_position = 0;
    m_buffer_length = *read_size;
    m_is_at_end = m_buffer_length == 0;
  }
}

pure fn BufferedLineReader::get_line() const wontthrow -> StringView
{
  return m_line.view();
}

fn resolve_git_directory() throws -> Path
{
  let dir = Path::current_directory();
  loop
  {
    let head = dir.clone();
    head.push_component(".git");
    let git_dir = head.clone();
    if (let const dot_git = head.read_entire_file()) {
      let const pointer = dot_git->view();
      let const gitdir_prefix = StringView{"gitdir: "};
      if (pointer.starts_with(gitdir_prefix)) {
        let line = pointer.substring(gitdir_prefix.length);
        while (!line.is_empty() &&
               (line[line.length - 1] == '\n' || line[line.length - 1] == '\r'))
        {
          line = line.substring_of_length(0, line.length - 1);
        }
        let resolved_gitdir = Path{line};
        if (!resolved_gitdir.is_absolute()) {
          resolved_gitdir = dir;
          resolved_gitdir.push_component(line);
        }
        git_dir = steal(resolved_gitdir);
      }
    }
    if (git_dir.is_directory()) return git_dir;

    let parent = dir.clone();
    parent.push_component("..");
    let normalized = parent.to_absolute().normalized();
    if (normalized.text() == dir.text()) break;
    dir = steal(normalized);
  }
  return Path{StringView{}};
}

fn current_git_branch() throws -> String
{
  let const git_dir = resolve_git_directory();
  if (git_dir.text().is_empty()) return String{heap_allocator()};

  let git_head = git_dir.clone();
  git_head.push_component("HEAD");
  if (let const content = git_head.read_entire_file()) {
    let text = content->view();
    while (!text.is_empty() &&
           (text[text.length - 1] == '\n' || text[text.length - 1] == '\r'))
    {
      text = text.substring_of_length(0, text.length - 1);
    }
    let const ref_prefix = StringView{"ref: refs/heads/"};
    if (text.starts_with(ref_prefix))
      return String{text.substring(ref_prefix.length)};
    return String{
        text.substring_of_length(0, text.length < 7 ? text.length : 7)};
  }
  return String{heap_allocator()};
}

fn read_git_ref_sha(const Path &git_dir, StringView ref_name) throws -> String
{
  let ref_path = git_dir.clone();
  ref_path.push_component(ref_name);
  if (let const content = ref_path.read_entire_file()) {
    let text = content->view();
    while (!text.is_empty() &&
           (text[text.length - 1] == '\n' || text[text.length - 1] == '\r'))
    {
      text = text.substring_of_length(0, text.length - 1);
    }
    if (!text.is_empty()) return String{text};
  }

  let packed_path = git_dir.clone();
  packed_path.push_component("packed-refs");
  if (let const packed = packed_path.read_entire_file()) {
    let search_prefix = String{heap_allocator()};
    search_prefix += ref_name;
    search_prefix += ' ';
    let remainder = packed->view();
    while (!remainder.is_empty()) {
      let const newline_pos = remainder.find_character('\n');
      let const line = newline_pos.has_value()
                           ? remainder.substring_of_length(0, *newline_pos)
                           : remainder;
      if (line.starts_with(search_prefix.view())) {
        let sha = line.substring(search_prefix.count());
        return String{sha};
      }
      if (!newline_pos.has_value()) break;
      remainder = remainder.substring(*newline_pos + 1);
    }
  }
  return String{heap_allocator()};
}

fn git_upstream_ref(const Path &git_dir, StringView branch_name) throws
    -> String
{
  let config_path = git_dir.clone();
  config_path.push_component("config");
  if (!config_path.exists()) return String{heap_allocator()};

  let const content = config_path.read_entire_file();
  if (!content.has_value()) return String{heap_allocator()};

  let const section_header =
      StringView{"[branch \""} + branch_name + StringView{"\"]"};
  let remote_name = String{heap_allocator()};
  let merge_ref = String{heap_allocator()};
  let in_section = false;

  let remainder = content->view();
  while (!remainder.is_empty()) {
    let const newline_pos = remainder.find_character('\n');
    let line = newline_pos.has_value()
                   ? remainder.substring_of_length(0, *newline_pos)
                   : remainder;

    while (!line.is_empty() && (line[0] == ' ' || line[0] == '\t'))
      line = line.substring(1);
    while (!line.is_empty() &&
           (line[line.length - 1] == ' ' || line[line.length - 1] == '\r'))
    {
      line = line.substring_of_length(0, line.length - 1);
    }

    if (line.starts_with("[")) {
      in_section = line == section_header;
    } else if (in_section) {
      let const eq_pos = line.find_character('=');
      if (!eq_pos.has_value()) {
        if (!newline_pos.has_value()) break;
        remainder = remainder.substring(*newline_pos + 1);
        continue;
      }
      let key = line.substring_of_length(0, *eq_pos);
      while (!key.is_empty() && key[key.length - 1] == ' ')
        key = key.substring_of_length(0, key.length - 1);
      let value = line.substring(*eq_pos + 1);
      while (!value.is_empty() && (value[0] == ' ' || value[0] == '\t'))
        value = value.substring(1);

      if (key == "remote") remote_name = String{value};
      if (key == "merge") merge_ref = String{value};
    }

    if (!newline_pos.has_value()) break;
    remainder = remainder.substring(*newline_pos + 1);
  }

  if (remote_name.is_empty() || merge_ref.is_empty())
    return String{heap_allocator()};

  let const refs_prefix = StringView{"refs/"};
  let const remotes_prefix = StringView{"refs/remotes/"};
  if (merge_ref.starts_with(refs_prefix)) {
    let short_ref = merge_ref.view().substring(refs_prefix.length);
    if (short_ref.starts_with("heads/")) short_ref = short_ref.substring(6);
    let result = String{heap_allocator()};
    result += remotes_prefix;
    result += remote_name.view();
    result += "/";
    result += short_ref;
    return result;
  }

  let result = String{heap_allocator()};
  result += remotes_prefix;
  result += remote_name.view();
  result += "/";
  result += merge_ref.view();
  return result;
}

fn git_ahead_behind_counts(i32 &ahead_count, i32 &behind_count) throws -> void
{
  let branch = String{heap_allocator()};
  git_status(branch, ahead_count, behind_count);
}

fn git_status(String &branch, i32 &ahead_count, i32 &behind_count) throws
    -> void
{
  branch.clear();
  ahead_count = 0;
  behind_count = 0;

  let const git_dir = resolve_git_directory();
  if (git_dir.text().is_empty()) return;

  let git_head = git_dir.clone();
  git_head.push_component("HEAD");
  let const head_content = git_head.read_entire_file();
  if (!head_content.has_value()) return;

  let head_text = head_content->view();
  while (!head_text.is_empty() && (head_text[head_text.length - 1] == '\n' ||
                                   head_text[head_text.length - 1] == '\r'))
  {
    head_text = head_text.substring_of_length(0, head_text.length - 1);
  }
  let const ref_prefix = StringView{"ref: refs/heads/"};
  branch = head_text.starts_with(ref_prefix)
               ? String{head_text.substring(ref_prefix.length)}
               : String{head_text.substring_of_length(
                     0, head_text.length < 7 ? head_text.length : 7)};
  if (branch.is_empty()) return;

  let const local_ref = StringView{"refs/heads/"} + branch.view();
  let const local_sha = read_git_ref_sha(git_dir, local_ref.view());
  if (local_sha.is_empty()) return;

  let const upstream = git_upstream_ref(git_dir, branch.view());
  if (upstream.is_empty()) return;

  let const upstream_sha = read_git_ref_sha(git_dir, upstream.view());
  if (upstream_sha.is_empty()) return;

  struct ahead_behind_cache
  {
    String branch{heap_allocator()};
    String local_sha{heap_allocator()};
    String upstream_sha{heap_allocator()};
    i32 ahead_count{0};
    i32 behind_count{0};
  };
  static ahead_behind_cache cached;
  if (cached.branch == branch && cached.local_sha == local_sha &&
      cached.upstream_sha == upstream_sha)
  {
    ahead_count = cached.ahead_count;
    behind_count = cached.behind_count;
    return;
  }

  ahead_count = 0;
  behind_count = 0;

  if (local_sha == upstream_sha) {
    cached.branch = String{branch.view()};
    cached.local_sha = String{local_sha.view()};
    cached.upstream_sha = String{upstream_sha.view()};
    cached.ahead_count = 0;
    cached.behind_count = 0;
    return;
  }

  let const path_env = os::get_environment_variable("PATH");
  if (!path_env.has_value()) return;
  let path_resolver = ProgramResolver{*path_env};
  let const git_results =
      path_resolver.search("git", ProgramResolver::SearchMode::First,
                           ProgramResolver::Requirement::Regular,
                           ProgramResolver::CachePolicy::Bypass);
  if (git_results.is_empty()) return;
  let const git_path = String{heap_allocator(), git_results[0].text().view()};

  let count_argv = ArrayList<String>{heap_allocator()};
  count_argv.push(String{heap_allocator(), git_path.view()});
  count_argv.push(String{heap_allocator(), "rev-list"});
  count_argv.push(String{heap_allocator(), "--left-right"});
  count_argv.push(String{heap_allocator(), "--count"});
  count_argv.push(local_sha.view() + "..." + upstream_sha.view());

  let const count_output =
      os::capture_program_output(count_argv, 5'000'000'000);

  let const parse_count = [](StringView s) -> i32 {
    while (!s.is_empty() &&
           (s[0] == ' ' || s[0] == '\t' || s[0] == '\n' || s[0] == '\r'))
    {
      s = s.substring(1);
    }
    if (s.is_empty()) return 0;
    i32 value = 0;
    for (usize i = 0; i < s.length; i++) {
      if (s[i] < '0' || s[i] > '9') break;
      value = value * 10 + (s[i] - '0');
    }
    return value;
  };

  if (count_output.has_value()) {
    let const separator = count_output->view().find_character('\t');
    if (separator.has_value()) {
      ahead_count =
          parse_count(count_output->view().substring_of_length(0, *separator));
      behind_count =
          parse_count(count_output->view().substring(*separator + 1));
    }
  }

  cached.branch = String{branch.view()};
  cached.local_sha = String{local_sha.view()};
  cached.upstream_sha = String{upstream_sha.view()};
  cached.ahead_count = ahead_count;
  cached.behind_count = behind_count;
}

} /* namespace utils */

} /* namespace koshka */
