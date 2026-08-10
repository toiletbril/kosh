#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-rRf] [--dry-run] path ...");

HELP_DESCRIPTION_DECL("The rm utility removes each path.");

FLAG(RM_RECURSIVE_R, Bool, 'r', "", "Remove directories and their contents.");
FLAG(RM_RECURSIVE_UPPER, Bool, 'R', "",
     "Remove directories and their contents.");
FLAG(RM_FORCE, Bool, 'f', "", "Ignore a missing path and never prompt.");
FLAG(RM_DRY_RUN, Bool, '\0', "dry-run",
     "Print what would be removed without removing anything.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Rm);

namespace koshka {

namespace koshkit {

fn remove_path(StringView path, removal_mode mode) throws -> bool
{
  let const is_recursive = mode == removal_mode::Recursive;
  let const target = Path{path};
  if (is_recursive && target.is_directory() && !target.is_symbolic_link()) {
    Maybe<ArrayList<String>> names = Path::read_directory(target);
    if (names.has_value())
      for (const String &name : *names) {
        let const child = PathBuilder{path}.append(name.view()).build();
        if (!remove_path(child.text().view(), mode)) return false;
      }
    return os::remove_directory(path);
  }
  return os::remove_file(path);
}

static fn report_dry_run_removal(const ExecContext &ec, EvalContext &cxt,
                                 StringView path, removal_mode mode) throws
    -> void
{
  let const is_recursive = mode == removal_mode::Recursive;
  let const target = Path{path};
  if (is_recursive && target.is_directory() && !target.is_symbolic_link()) {
    if (Maybe<ArrayList<String>> names = Path::read_directory(target);
        names.has_value())
    {
      for (const String &name : *names) {
        let const child = PathBuilder{path}.append(name.view()).build();
        report_dry_run_removal(ec, cxt, child.text().view(), mode);
      }
    }
  }

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
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  let const should_force = FLAG_RM_FORCE.is_enabled();
  let const is_recursive =
      FLAG_RM_RECURSIVE_R.is_enabled() || FLAG_RM_RECURSIVE_UPPER.is_enabled();
  let const is_dry_run = FLAG_RM_DRY_RUN.is_enabled();

  if (operands.is_empty() && !should_force) {
    return report_usage_error(ec, cxt, args[0].view());
  }

  i32 status = 0;
  for (const String &operand : operands) {
    if (names_dot_or_dotdot(operand.view())) {
      report_soft_koshkit_error(ec, cxt,
                                "rm: refusing to remove '.' or '..' directory: "
                                "skipping '" +
                                    operand + "'");
      status = 1;
      continue;
    }

    if (names_root_directory(operand.view())) {
      report_soft_koshkit_error(
          ec, cxt,
          "rm: refusing to remove the root directory: skipping '" + operand +
              "'");
      status = 1;
      continue;
    }

    let const target = Path{operand.view()};
    if (!target.exists() && !target.is_symbolic_link()) {
      if (should_force) continue;
      report_soft_koshkit_error(ec, cxt,
                                "rm: cannot remove '" + operand +
                                    "': no such file or directory");
      status = 1;
      continue;
    }
    if (is_dry_run) {
      report_dry_run_removal(ec, cxt, operand.view(),
                             is_recursive ? removal_mode::Recursive
                                          : removal_mode::SinglePath);
      continue;
    }

    if (!remove_path(operand.view(), is_recursive ? removal_mode::Recursive
                                                  : removal_mode::SinglePath))
    {
      report_soft_koshkit_error(ec, cxt,
                                "rm: cannot remove '" + operand +
                                    "': " + os::last_system_error_message());
      status = 1;
    }
  }
  return status;
}

} // namespace koshkit

} // namespace koshka
