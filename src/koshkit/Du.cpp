/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the du utility in koshkit. The
 * du utility prints the total byte size of each path.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-sh] [path ...]");

HELP_DESCRIPTION_DECL(
    "The du utility prints the total byte size of each path.");

FLAG(DU_SUMMARY, Bool, 's', "",
     "Print only the total for each path, the default.");
FLAG(DU_HUMAN, Bool, 'h', "",
     "Print the size in a human-readable form such as 4.0K or 1.5M.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Du);

namespace koshka {

namespace koshkit {

/* A symlink is counted as its own size and not followed, so a cycle cannot
   run forever. */
static fn total_size(const Path &path, Path &failed_path,
                     String &failure_message) throws -> Maybe<u64>
{
  if (path.is_directory() && !path.is_symbolic_link()) {
    u64 total_bytes = 0;
    Maybe<ArrayList<String>> names = Path::read_directory(path);
    if (!names.has_value()) {
      failure_message = os::last_system_error_message();
      failed_path = path.clone();
      return None;
    }

    for (let const &name : *names) {
      if (os::INTERRUPT_REQUESTED) return None;

      let const child =
          PathBuilder{path.text().view()}.append(name.view()).build();
      let const child_size = total_size(child, failed_path, failure_message);
      if (!child_size.has_value()) return None;
      if (*child_size > UINT64_MAX - total_bytes) {
        failure_message = "the total size is too large";
        failed_path = child.clone();
        return None;
      }
      total_bytes += *child_size;
    }

    return total_bytes;
  }

  let const size = path.file_size();
  if (!size.has_value()) {
    failure_message = os::last_system_error_message();
    failed_path = path.clone();
  }
  return size;
}

Du::Du() = default;

pure fn Du::kind() const wontthrow -> Utility::Kind { return Kind::Du; }

fn Du::execute(const ExecContext &ec, EvalContext &cxt,
               const ArrayList<String> &args,
               const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  ArrayList<StringView> targets{cxt.scratch_allocator()};
  if (operands.is_empty())
    targets.push(StringView{"."});
  else
    for (let const &operand : operands)
      targets.push(operand.view());

  let output = String{cxt.scratch_allocator()};
  i32 status = 0;
  for (let const &target : targets) {
    let const path = Path{target};
    if (!path.exists()) {
      report_soft_koshkit_error(ec, cxt,
                                "du: cannot access '" +
                                    String{cxt.scratch_allocator(), target} +
                                    "': no such file or directory");
      status = 1;
      continue;
    }
    let failed_path = Path{};
    let failure_message = String{cxt.scratch_allocator()};
    let const total = total_size(path, failed_path, failure_message);
    if (os::INTERRUPT_REQUESTED) return 130;
    if (!total.has_value()) {
      report_soft_koshkit_error(ec, cxt,
                                "du: cannot read '" + failed_path.text() +
                                    "': " + failure_message);
      status = 1;
      continue;
    }
    output += FLAG_DU_HUMAN.is_enabled()
                  ? format_human_size(*total, cxt.scratch_allocator())
                  : String::from(*total, cxt.scratch_allocator());
    output += '\t';
    output += target;
    output += '\n';
  }

  ec.print_to_stdout(output);
  return status;
}

} /* namespace koshkit */

} /* namespace koshka */
