#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-amnrsv]");

HELP_DESCRIPTION_DECL("The uname utility writes system identification.");

FLAG(UNAME_ALL, Bool, 'a', "all", "Write all fields.");
FLAG(UNAME_MACHINE, Bool, 'm', "machine", "Write the machine type.");
FLAG(UNAME_NODE, Bool, 'n', "nodename", "Write the network node name.");
FLAG(UNAME_RELEASE, Bool, 'r', "release",
     "Write the operating system release.");
FLAG(UNAME_SYSTEM, Bool, 's', "kernel-name",
     "Write the operating system name.");
FLAG(UNAME_VERSION, Bool, 'v', "kernel-version",
     "Write the operating system version.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Uname);

namespace koshka::koshkit {

Uname::Uname() = default;

pure fn Uname::kind() const wontthrow -> Utility::Kind { return Kind::Uname; }

fn Uname::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty()) return report_usage_error(ec, cxt, args[0].view());
  let const is_all = FLAG_UNAME_ALL.is_enabled();
  bool should_system = is_all || FLAG_UNAME_SYSTEM.is_enabled();
  let const should_node = is_all || FLAG_UNAME_NODE.is_enabled();
  let const should_release = is_all || FLAG_UNAME_RELEASE.is_enabled();
  let const should_version = is_all || FLAG_UNAME_VERSION.is_enabled();
  let const should_machine = is_all || FLAG_UNAME_MACHINE.is_enabled();
  if (!should_system && !should_node && !should_release && !should_version &&
      !should_machine)
    should_system = true;

  let output = String{cxt.scratch_allocator()};
  let const do_append = [&](StringView value) throws -> void {
    if (!output.is_empty()) output += ' ';
    output += value;
  };
  if (should_system) do_append(os::executable_system_name().view());
  if (should_node) {
    let const hostname = os::get_hostname();
    do_append(hostname.has_value() ? hostname->view() : StringView{"unknown"});
  }
  if (should_release) do_append(os::system_release_name().view());
  if (should_version) do_append(os::system_version_name().view());
  if (should_machine) do_append(os::executable_machine_name().view());
  output += '\n';
  ec.print_to_stdout(output);
  return 0;
}

} // namespace koshka::koshkit
