/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the rm utility. It applies force and interactive
 * policies, recursively removes directory trees, rejects protected operands,
 * and supports dry-run reporting.
 */

#include "../CLI.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-fiRr] [--dry-run] path ...");

HELP_DESCRIPTION_DECL("The rm utility removes each path.");

FLAG(RM_RECURSIVE_R, Bool, 'r', "", "Remove directories and their contents.");
FLAG(RM_RECURSIVE_UPPER, Bool, 'R', "",
     "Remove directories and their contents.");
FLAG(RM_FORCE, Bool, 'f', "", "Ignore a missing path and never prompt.");
FLAG(RM_INTERACTIVE, Bool, 'i', "", "Ask before each removal.");
FLAG(RM_DRY_RUN, Bool, '\0', "dry-run",
     "Print what would be removed without removing anything.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Rm);

namespace koshka {

namespace koshkit {

fn remove_path(StringView path, removal_mode mode, Allocator allocator,
               Path::entry_kind known_kind = Path::entry_kind::Unknown) throws
    -> bool
{
  let const is_recursive = mode == removal_mode::Recursive;
  let const target = Path{path};
  let is_directory = false;
  let is_symbolic_link = false;
  if (known_kind == Path::entry_kind::Unknown) {
    is_directory = target.is_directory();
    is_symbolic_link = target.is_symbolic_link();
  } else {
    is_directory = known_kind == Path::entry_kind::Directory;
    is_symbolic_link = known_kind == Path::entry_kind::Symlink;
  }
  if (is_recursive && is_directory && !is_symbolic_link) {
    let names = os::list_directory_status(path, allocator);
    if (names.has_value())
      for (let const &entry : *names) {
        if (os::INTERRUPT_REQUESTED) return false;
        let child = Path{path, allocator};
        child.append(entry.child.name.view());
        if (!remove_path(child.text().view(), mode, allocator, entry.child.kind))
          return false;
      }
    return os::remove_directory(path);
  }
  return os::remove_file(path);
}

static fn remove_path_with_prompt(const ExecContext &ec, StringView path,
                                  removal_mode mode, bool should_prompt,
                                  Allocator allocator,
                                  Path::entry_kind known_kind =
                                      Path::entry_kind::Unknown) throws
    -> bool
{
  if (!should_prompt) return remove_path(path, mode, allocator, known_kind);

  let const is_recursive = mode == removal_mode::Recursive;
  let const target = Path{path};
  let is_directory = false;
  let is_symbolic_link = false;
  if (known_kind == Path::entry_kind::Unknown) {
    is_directory = target.is_directory();
    is_symbolic_link = target.is_symbolic_link();
  } else {
    is_directory = known_kind == Path::entry_kind::Directory;
    is_symbolic_link = known_kind == Path::entry_kind::Symlink;
  }
  if (is_recursive && is_directory && !is_symbolic_link) {
    let names = os::list_directory_status(path, allocator);
    if (names.has_value())
      for (let const &entry : *names) {
        if (os::INTERRUPT_REQUESTED) return false;
        let child = Path{path, allocator};
        child.append(entry.child.name.view());
        if (!remove_path_with_prompt(ec, child.text().view(), mode,
                                     should_prompt, allocator,
                                     entry.child.kind))
          return false;
      }
    if (!confirm_koshkit_action(ec, "rm: remove '" + String{path} + "'? "))
      return true;
    return os::remove_directory(path);
  }
  if (!confirm_koshkit_action(ec, "rm: remove '" + String{path} + "'? "))
    return true;
  return os::remove_file(path);
}

static fn report_dry_run_removal(const ExecContext &ec, EvalContext &cxt,
                                 StringView path, removal_mode mode,
                                 bool should_prompt, Allocator allocator,
                                 Path::entry_kind known_kind =
                                     Path::entry_kind::Unknown) throws
    -> void
{
  let const is_recursive = mode == removal_mode::Recursive;
  let const target = Path{path};
  let is_directory = false;
  let is_symbolic_link = false;
  if (known_kind == Path::entry_kind::Unknown) {
    is_directory = target.is_directory();
    is_symbolic_link = target.is_symbolic_link();
  } else {
    is_directory = known_kind == Path::entry_kind::Directory;
    is_symbolic_link = known_kind == Path::entry_kind::Symlink;
  }
  if (is_recursive && is_directory && !is_symbolic_link) {
    if (let names = os::list_directory_status(path, allocator);
        names.has_value())
    {
      for (let const &entry : *names) {
        if (os::INTERRUPT_REQUESTED) return;
        let child = Path{path, allocator};
        child.append(entry.child.name.view());
        report_dry_run_removal(ec, cxt, child.text().view(), mode,
                               should_prompt, allocator, entry.child.kind);
      }
    }
  }

  if (should_prompt &&
      !confirm_koshkit_action(ec, "rm: remove '" + String{path} + "'? "))
    return;

  ec.print_to_stdout("rm: would remove '" +
                     String{cxt.scratch_allocator(), path} + "'\n");
}

/* POSIX requires rm to refuse a . or .. operand even under -f. */
static fn names_dot_or_dotdot(StringView operand) wontthrow -> bool
{
  usize end = operand.length;
  while (end > 1 && operand[end - 1] == '/')
    end--;

  usize start = end;
  while (start > 0 && operand[start - 1] != '/')
    start--;

  let const base = operand.substring_of_length(start, end - start);
  return base == StringView{"."} || base == StringView{".."};
}

/* rm refuses / even under -f, matching GNU preserve-root. */
static fn names_root_directory(StringView operand) wontthrow -> bool
{
  if (operand.length == 0) return false;

  for (usize i = 0; i < operand.length; i++)
    if (operand[i] != '/') return false;

  return true;
}

Rm::Rm() = default;

pure fn Rm::kind() const wontthrow -> Utility::Kind { return Kind::Rm; }

fn Rm::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const[operands, operand_locations] = parse_util_operands(
      FLAG_LIST, args, cxt.scratch_allocator(), &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const should_force = FLAG_RM_FORCE.is_enabled();
  let const should_prompt = FLAG_RM_INTERACTIVE.is_enabled() &&
                            (!should_force || FLAG_RM_INTERACTIVE.position() >
                                                  FLAG_RM_FORCE.position());
  let const is_recursive =
      FLAG_RM_RECURSIVE_R.is_enabled() || FLAG_RM_RECURSIVE_UPPER.is_enabled();
  let const is_dry_run = FLAG_RM_DRY_RUN.is_enabled();
  let const allocator = cxt.scratch_allocator();

  if (operands.is_empty() && !should_force) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  i32 status = 0;
  for (const String &operand : operands) {
    if (os::INTERRUPT_REQUESTED) return 130;
    if (names_dot_or_dotdot(operand.view())) {
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          "refusing to remove '.' or '..' directory: "
          "skipping '" +
              operand + "'");
      status = 1;
      continue;
    }

    if (names_root_directory(operand.view())) {
      report_soft_koshkit_util_error(
          ec, cxt, args[0].view(),
          "refusing to remove the root directory: skipping '" + operand + "'");
      status = 1;
      continue;
    }

    let const target = Path{operand.view()};
    if (!target.exists() && !target.is_symbolic_link()) {
      if (should_force) continue;
      report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                     "cannot remove '" + operand +
                                         "': no such file or directory");
      status = 1;
      continue;
    }
    if (is_dry_run) {
      report_dry_run_removal(ec, cxt, operand.view(),
                             is_recursive ? removal_mode::Recursive
                                          : removal_mode::SinglePath,
                             should_prompt, allocator);
      continue;
    }

    if (!remove_path_with_prompt(ec, operand.view(),
                                 is_recursive ? removal_mode::Recursive
                                              : removal_mode::SinglePath,
                                 should_prompt, allocator))
    {
      report_soft_koshkit_util_error(ec, cxt, args[0].view(),
                                     "cannot remove '" + operand + "': " +
                                         os::last_system_error_message());
      status = 1;
    }
    if (os::INTERRUPT_REQUESTED) return 130;
  }
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
