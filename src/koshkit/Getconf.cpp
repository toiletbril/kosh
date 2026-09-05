/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the getconf utility in
 * koshkit. The getconf utility writes configuration values.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-v specification] system-variable | path-variable path");

HELP_DESCRIPTION_DECL("The getconf utility writes configuration values.");

FLAG(GETCONF_SPECIFICATION, String, 'v', "specification",
     "Use this POSIX specification.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Getconf);

namespace koshka::koshkit {

inline constexpr static_string_entry<os::system_configuration_key>
    SYSTEM_CONFIGURATION_ENTRIES[] = {
        {SSK("ARG_MAX"),        os::system_configuration_key::ArgMax      },
        {SSK("CHILD_MAX"),      os::system_configuration_key::ChildMax    },
        {SSK("CLK_TCK"),        os::system_configuration_key::ClockTicks  },
        {SSK("NGROUPS_MAX"),    os::system_configuration_key::GroupsMax   },
        {SSK("OPEN_MAX"),       os::system_configuration_key::OpenMax     },
        {SSK("PAGESIZE"),       os::system_configuration_key::PageSize    },
        {SSK("PAGE_SIZE"),      os::system_configuration_key::PageSize    },
        {SSK("STREAM_MAX"),     os::system_configuration_key::StreamMax   },
        {SSK("_POSIX_VERSION"), os::system_configuration_key::PosixVersion},
};
inline constexpr StaticStringMap SYSTEM_CONFIGURATIONS{
    SYSTEM_CONFIGURATION_ENTRIES};

inline constexpr static_string_entry<os::path_configuration_key>
    PATH_CONFIGURATION_ENTRIES[] = {
        {SSK("LINK_MAX"),                os::path_configuration_key::LinkMax         },
        {SSK("MAX_CANON"),               os::path_configuration_key::MaxCanonical    },
        {SSK("MAX_INPUT"),               os::path_configuration_key::MaxInput        },
        {SSK("NAME_MAX"),                os::path_configuration_key::NameMax         },
        {SSK("PATH_MAX"),                os::path_configuration_key::PathMax         },
        {SSK("PIPE_BUF"),                os::path_configuration_key::PipeBuffer      },
        {SSK("_POSIX_CHOWN_RESTRICTED"),
         os::path_configuration_key::ChownRestricted                                 },
        {SSK("_POSIX_NO_TRUNC"),         os::path_configuration_key::NoTrunc         },
        {SSK("_POSIX_VDISABLE"),         os::path_configuration_key::DisableCharacter},
};
inline constexpr StaticStringMap PATH_CONFIGURATIONS{
    PATH_CONFIGURATION_ENTRIES};

Getconf::Getconf() = default;

pure fn Getconf::kind() const wontthrow -> Utility::Kind
{
  return Kind::Getconf;
}

fn Getconf::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      parse_util_operands(FLAG_LIST, args, &arg_locations, &operand_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.is_empty() || operands.count() > 2)
    return report_usage_error(ec, cxt, args[0].view());
  if (FLAG_GETCONF_SPECIFICATION.is_set() &&
      FLAG_GETCONF_SPECIFICATION.value() != "POSIX_V7_LP64_OFF64" &&
      FLAG_GETCONF_SPECIFICATION.value() != "POSIX_V7_ILP32_OFFBIG")
  {
    report_soft_koshkit_util_error(
        ec, cxt, FLAG_GETCONF_SPECIFICATION.value_location(), args[0].view(),
        "unsupported specification '" +
            String{FLAG_GETCONF_SPECIFICATION.value()} + "'");
    return 2;
  }

  Maybe<i64> value;
  if (let const system_key = SYSTEM_CONFIGURATIONS.find(operands[0].view());
      system_key.has_value())
  {
    if (operands.count() != 1)
      return report_usage_error(ec, cxt, args[0].view());
    value = os::system_configuration(*system_key);
  } else if (let const path_key = PATH_CONFIGURATIONS.find(operands[0].view());
             path_key.has_value())
  {
    if (operands.count() != 2)
      return report_usage_error(ec, cxt, args[0].view());
    value = os::path_configuration(operands[1].view(), *path_key);
  } else {
    report_soft_koshkit_util_error(ec, cxt, operand_locations[0],
                                   args[0].view(),
                                   "unknown variable '" + operands[0] + "'");
    return 1;
  }

  if (!value.has_value() || *value == -1)
    ec.print_to_stdout("undefined\n");
  else
    ec.print_to_stdout(String::from(*value, cxt.scratch_allocator()) + "\n");
  return 0;
}

} // namespace koshka::koshkit
