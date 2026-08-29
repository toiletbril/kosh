#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../StaticStringMap.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-is] [-f file] [-p priority] [-t tag] [message ...]");

HELP_DESCRIPTION_DECL("The logger utility writes a message to the system log.");

FLAG(LOGGER_PID, Bool, 'i', "id", "Include the process identifier.");
FLAG(LOGGER_FILE, String, 'f', "file", "Read the message from this file.");
FLAG(LOGGER_PRIORITY, String, 'p', "priority",
     "Use this facility and severity.");
FLAG(LOGGER_STDERR, Bool, 's', "stderr", "Copy the message to standard error.");
FLAG(LOGGER_TAG, String, 't', "tag", "Use this message tag.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Logger);

namespace koshka::koshkit {

constexpr PackedStringKey LOGGER_FACILITY_KEYS[] = {
    SSK("auth"),   SSK("authpriv"), SSK("cron"),   SSK("daemon"), SSK("ftp"),
    SSK("kern"),   SSK("local0"),   SSK("local1"), SSK("local2"), SSK("local3"),
    SSK("local4"), SSK("local5"),   SSK("local6"), SSK("local7"), SSK("lpr"),
    SSK("mail"),   SSK("news"),     SSK("syslog"), SSK("user"),   SSK("uucp")};

constexpr StaticStringSet LOGGER_FACILITIES{LOGGER_FACILITY_KEYS};

constexpr PackedStringKey LOGGER_SEVERITY_KEYS[] = {
    SSK("alert"), SSK("crit"), SSK("debug"),  SSK("emerg"),
    SSK("err"),   SSK("info"), SSK("notice"), SSK("warning")};

constexpr StaticStringSet LOGGER_SEVERITIES{LOGGER_SEVERITY_KEYS};

static pure fn logger_priority_is_valid(StringView priority) wontthrow -> bool
{
  let severity = priority;
  if (let const separator = priority.find_character('.'); separator.has_value())
  {
    let const facility = priority.substring_of_length(0, *separator);
    severity = priority.substring(*separator + 1);
    if (!LOGGER_FACILITIES.contains(facility)) return false;
  }

  return LOGGER_SEVERITIES.contains(severity);
}

Logger::Logger() = default;

pure fn Logger::kind() const wontthrow -> Utility::Kind { return Kind::Logger; }

fn Logger::execute(const ExecContext &ec, EvalContext &cxt,
                   const ArrayList<String> &args,
                   const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (FLAG_LOGGER_FILE.is_set() && !operands.is_empty())
    return report_usage_error(ec, cxt, args[0].view());
  let message = String{cxt.scratch_allocator()};
  if (FLAG_LOGGER_FILE.is_set()) {
    let const content = read_named_or_stdin(ec, FLAG_LOGGER_FILE.value());
    if (!content.has_value()) {
      report_soft_koshkit_util_error(
          ec, cxt, FLAG_LOGGER_FILE.value_location(), args[0].view(),
          "cannot read '" + String{FLAG_LOGGER_FILE.value()} +
              "': " + os::last_system_error_message());
      return 1;
    }
    message = content->clone();
  } else if (operands.is_empty()) {
    let const content = read_fd_to_string(ec.in_fd.value_or(KOSH_STDIN));
    if (!content.has_value()) return 1;
    message = content->clone();
  } else {
    for (usize index = 0; index < operands.count(); index++) {
      if (index != 0) message += ' ';
      message += operands[index].view();
    }
  }
  while (!message.is_empty() && message.back() == '\n')
    message.pop_back();
  let const tag =
      FLAG_LOGGER_TAG.is_set() ? FLAG_LOGGER_TAG.value() : StringView{"logger"};
  let const priority = FLAG_LOGGER_PRIORITY.is_set()
                           ? FLAG_LOGGER_PRIORITY.value()
                           : StringView{"user.notice"};
  if (!logger_priority_is_valid(priority)) {
    report_soft_koshkit_util_error(
        ec, cxt, FLAG_LOGGER_PRIORITY.value_location(), args[0].view(),
        "invalid priority '" + String{priority} + "'");
    return 1;
  }
  if (!os::write_system_log(tag, priority, message.view(),
                            FLAG_LOGGER_PID.is_enabled(),
                            FLAG_LOGGER_STDERR.is_enabled()))
  {
    let const location = FLAG_LOGGER_PRIORITY.is_set()
                             ? FLAG_LOGGER_PRIORITY.value_location()
                             : arg_locations[0];
    report_soft_koshkit_util_error(ec, cxt, location, args[0].view(),
                                   "cannot write to the system log: " +
                                       os::last_system_error_message());
    return 1;
  }

  return 0;
}

} // namespace koshka::koshkit
