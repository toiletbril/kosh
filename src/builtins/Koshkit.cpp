/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements bundled utility selection, help and sorted listings,
 * direct bare-name dispatch, and multicall symlink installation for the
 * koshkit builtin.
 */

#include "../Koshkit.hpp"

#include "../Builtin.hpp"
#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Utils.hpp"
#include "../base/Trace.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[--color when] [utility] [arg ...]");

HELP_DESCRIPTION_DECL("The koshkit builtin runs a bundled utility.");

FLAG(HELP, Bool, '\0', "help", "Display help and list the utilities.");
FLAG(KOSHKIT_LIST, Bool, '\0', "list", "List the utility names, one per line.");
FLAG(KOSHKIT_ASSIMILATE, String, '\0', "assimilate",
     "Install a symlink to this binary for each utility into the given "
     "directory.");
FLAG(KOSHKIT_COLOR, String, '\0', "color",
     "Set utility color output to always, auto, or never.");

REGISTER_BUILTIN_FLAGS(Koshkit);

namespace koshka {

Koshkit::Koshkit() = default;

pure fn Koshkit::kind() const wontthrow -> Builtin::Kind
{
  return Kind::Koshkit;
}

enum class utility_section : u8
{
  Posix,
  Koshka,
};

static pure fn get_utility_section(koshkit::Utility::Kind kind) wontthrow
    -> utility_section
{
  switch (kind) {
  case koshkit::Utility::Kind::Evil:
  case koshkit::Utility::Kind::EvilFiles:
  case koshkit::Utility::Kind::EvilFS:
  case koshkit::Utility::Kind::EvilNet:
  case koshkit::Utility::Kind::GoodNode:
  case koshkit::Utility::Kind::EvilPS:
  case koshkit::Utility::Kind::GoodStat:
  case koshkit::Utility::Kind::EvilDisk:
  case koshkit::Utility::Kind::EvilIO:
  case koshkit::Utility::Kind::EvilLogs:
  case koshkit::Utility::Kind::EvilIso:
  case koshkit::Utility::Kind::GoodCore:
  case koshkit::Utility::Kind::EvilSS:
  case koshkit::Utility::Kind::GoodFSW: return utility_section::Koshka;
  default: return utility_section::Posix;
  }
}

fn Koshkit::execute(ExecContext &ec, EvalContext &cxt) const throws -> i32
{
  ASSERT(!ec.args().is_empty());

  defer { koshkit::set_koshkit_color_mode(cli_color_mode::Auto); };

  let const utility_index =
      ec.program() == "koshkit"
          ? parse_until_subcommand(FLAG_LIST, ec.args(), &ec.arg_locations(),
                                   nullptr, ec.program())
          : usize{0};
  defer { reset_flags(FLAG_LIST); };

  if (FLAG_KOSHKIT_COLOR.is_set()) {
    let const selected = parse_cli_color_mode(FLAG_KOSHKIT_COLOR.value());
    if (!selected.has_value()) {
      let error =
          ErrorWithLocation{FLAG_KOSHKIT_COLOR.value_location(),
                            "koshkit: invalid color mode '" +
                                String{FLAG_KOSHKIT_COLOR.value()} + "'"};
      error.set_command_status(2);
      throw error;
    }
    koshkit::set_koshkit_color_mode(*selected);
  }

  if (utility_index < ec.args().count()) {
    if (let const chosen = koshkit::find_util(ec.args()[utility_index].view());
        chosen.has_value())
      return koshkit::dispatch(ec, cxt, utility_index, chosen);
  }

  let const &sorted_names = koshkit::sorted_util_names();

  if (FLAG_KOSHKIT_LIST.is_enabled()) {
    let names_output = String{cxt.scratch_allocator()};
    for (let const &name : sorted_names) {
      names_output += name.view();
      names_output += '\n';
    }
    ec.print_to_stdout(names_output);
    return 0;
  }

  if (FLAG_KOSHKIT_ASSIMILATE.is_set()) {
    if (FLAG_KOSHKIT_ASSIMILATE.value().is_empty())
      return report_usage_error(ec, cxt, ec.program());

    let const target = os::current_executable_path();
    if (!target.has_value()) {
      report_soft_builtin_error(
          ec, cxt, "Cannot resolve this binary's path to assimilate");
      return 1;
    }

    if (!Path{FLAG_KOSHKIT_ASSIMILATE.value()}.is_directory()) {
      report_soft_builtin_error(
          ec, cxt,
          "Cannot assimilate into '" +
              String{cxt.scratch_allocator(), FLAG_KOSHKIT_ASSIMILATE.value()} +
              "': not a directory");
      return 1;
    }

    i32 status = 0;
    for (let const &name : sorted_names) {
      let link = Path{FLAG_KOSHKIT_ASSIMILATE.value()};
      link.push_component(name.view());
      if (link.is_symbolic_link()) os::remove_file(link.view());
      if (!os::create_symlink(target->view(), link.view())) {
        report_soft_builtin_error(ec, cxt,
                                  "Cannot link '" + link.text() +
                                      "': " + os::last_system_error_message());
        status = 1;
      }
    }
    return status;
  }

  if (FLAG_HELP.is_enabled() || utility_index == ec.args().count()) {
    let listing = String{cxt.scratch_allocator()};
    listing += "DESCRIPTION\n";
    listing += wrap_text(HELP_DESCRIPTION, HELP_INDENT, HELP_WRAP_WIDTH);
    listing += "\n\nSYNOPSIS\n";
    listing += "  koshkit [utility] [arg ...]\n";
    listing += "  koshkit --color when [utility] [arg ...]\n";
    listing += "  koshkit --list\n";
    listing += "  koshkit --assimilate DIR\n";
    listing += "\nUTILITIES\n\n";

    let posix_names = ArrayList<StringView>{cxt.scratch_allocator()};
    let koshka_names = ArrayList<StringView>{cxt.scratch_allocator()};
    for (let const &name : sorted_names) {
      let const kind = koshkit::find_util(name.view());
      ASSERT(kind.has_value());
      switch (get_utility_section(*kind)) {
      case utility_section::Posix: posix_names.push(name.view()); break;
      case utility_section::Koshka: koshka_names.push(name.view()); break;
      }
    }

    let const should_color = koshkit::koshkit_should_color();
    append_report_name_section(listing, "POSIX", posix_names, should_color,
                               "  ");
    append_report_name_section(listing, "Koshka", koshka_names, should_color,
                               "  ");
    listing.pop_back();

    ec.print_to_stdout(format_cli_help(listing.view()));
    return 0;
  }

  return koshkit::dispatch(ec, cxt, 1);
}

} // namespace koshka
