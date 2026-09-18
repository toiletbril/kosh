/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the evilnet utility. It reports assigned IPv4 and IPv6
 * interface addresses in deterministic content-derived columns.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-atlf] [--falloff seconds]");

HELP_DESCRIPTION_DECL(
    "The evilnet utility reports the addresses assigned to each interface.");

FLAG(EVILNET_ALL, Bool, 'a', "all", "Include interface traffic and TCP data.");
FLAG(EVILNET_TRAFFIC, Bool, 't', "traffic", "Show interface traffic only.");
static pure fn is_evilnet_sample_duration(koshka::StringView value) wontthrow
    -> bool
{
  return !value.is_empty() &&
         ((value[0] >= '0' && value[0] <= '9') || value[0] == '.');
}
FLAG_OPTIONAL(EVILNET_LIVE, 'l', "live",
              "Refresh live traffic at an optional interval; the default is "
              "0.5 seconds.",
              is_evilnet_sample_duration, "seconds");
FLAG_OPTIONAL(EVILNET_CUMULATIVE, '\0', "cumulative",
              "Measure traffic over an optional sampling window; the default "
              "is one second.",
              is_evilnet_sample_duration, "seconds");
FLAG(EVILNET_FAILURES, Bool, 'f', "failures",
     "Show TCP failure and packet-loss telemetry only.");
FLAG(EVILNET_FALLOFF, String, '\0', "falloff",
     "Retain inactive interfaces for this many seconds.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilNet);

namespace koshka::koshkit {

namespace {

pure fn family_name(os::network_address_family family) wontthrow -> StringView
{
  switch (family) {
  case os::network_address_family::IPv4: return "IPv4";
  case os::network_address_family::IPv6: return "IPv6";
  }

  unreachable("unknown network address family");
}

fn append_network_interface_report(String &output, bool should_color,
                                   StringView indentation) throws -> usize
{
  let addresses = os::network_interface_addresses();
  addresses.sort([](const os::network_interface_address &left,
                    const os::network_interface_address &right) {
    if (left.interface_name.view() != right.interface_name.view()) {
      return left.interface_name.view() < right.interface_name.view();
    }

    if (left.family != right.family) return left.family < right.family;

    return left.address.view() < right.address.view();
  });

  usize interface_width = 9;
  for (let const &address : addresses) {
    if (address.interface_name.length() > interface_width) {
      interface_width = address.interface_name.length();
    }
  }

  output += indentation;
  append_report_column(output, "NAME", interface_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  output += "  ";
  append_report_column(output, "FAMILY", 6, false, colors::ansi::BOLD_MAGENTA,
                       should_color);
  output += "  ";
  append_report_text(output, "ADDRESS", {}, should_color);
  output += "\n";

  for (let const &address : addresses) {
    output += indentation;
    append_report_column(output, address.interface_name.view(), interface_width,
                         false, colors::ansi::BOLD_GREEN, should_color);
    output += "  ";
    append_report_column(output, family_name(address.family), 6, false,
                         colors::ansi::BOLD_MAGENTA, should_color);
    output += "  ";
    append_report_text(output, address.address.view(), {}, should_color);
    output += "\n";
  }

  return addresses.count();
}

fn append_network_traffic_statistics_report(
    String &output, ArrayList<String> &warnings, Allocator allocator,
    const ArrayList<os::network_interface_statistics_entry> &statistics,
    bool should_color, bool is_sampled = false) throws -> usize
{
  usize name_width = 4;
  for (let const &entry : statistics) {
    if (entry.interface_name.length() > name_width) {
      name_width = entry.interface_name.length();
    }
  }

  output += "\n\n";
  append_report_column(output, "NAME", name_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  const StringView HEADERS[] = {
      is_sampled ? StringView{"RX/S"} : StringView{"RX"},
      is_sampled ? StringView{"TX/S"} : StringView{"TX"},
      is_sampled ? StringView{"RX PACKETS/S"} : StringView{"RX PACKETS"},
      is_sampled ? StringView{"TX PACKETS/S"} : StringView{"TX PACKETS"},
      is_sampled ? StringView{"RX ERRORS/S"} : StringView{"RX ERRORS"},
      is_sampled ? StringView{"TX ERRORS/S"} : StringView{"TX ERRORS"},
      is_sampled ? StringView{"RX DROPS/S"} : StringView{"RX DROPS"},
      is_sampled ? StringView{"TX DROPS/S"} : StringView{"TX DROPS"},
      "RX CAP",    "TX CAP",    "TX QUEUE",   "TX LIMIT",
  };
  constexpr usize WIDTHS[] = {9, 9, 12, 12, 10, 10, 9, 9, 9, 9, 10, 10};
  for (usize index = 0; index < countof(HEADERS); index++) {
    if (index != 0) output += "  ";
    append_report_column(output, HEADERS[index], WIDTHS[index], true,
                         colors::ansi::BOLD_CYAN, should_color);
  }
  output += "\n";

  for (let const &entry : statistics) {
    append_report_column(output, entry.interface_name.view(), name_width, false,
                         colors::ansi::BOLD_GREEN, should_color);
    output += "  ";
    append_report_column(
        output,
        entry.has_field(os::network_statistics_field::ReceiveBytes)
            ? format_human_size(entry.receive_bytes, allocator).view()
            : StringView{"-"},
        9, true, colors::ansi::GREEN, should_color);
    output += "  ";
    append_report_column(
        output,
        entry.has_field(os::network_statistics_field::TransmitBytes)
            ? format_human_size(entry.transmit_bytes, allocator).view()
            : StringView{"-"},
        9, true, colors::ansi::GREEN, should_color);
    const u64 counters[] = {entry.receive_packet_count,
                            entry.transmit_packet_count,
                            entry.receive_error_count,
                            entry.transmit_error_count,
                            entry.receive_drop_count,
                            entry.transmit_drop_count,
                            entry.receive_link_bits_per_second / 8,
                            entry.transmit_link_bits_per_second / 8,
                            entry.transmit_queue_length,
                            entry.transmit_queue_limit};
    constexpr os::network_statistics_field FIELDS[] = {
        os::network_statistics_field::ReceivePackets,
        os::network_statistics_field::TransmitPackets,
        os::network_statistics_field::ReceiveErrors,
        os::network_statistics_field::TransmitErrors,
        os::network_statistics_field::ReceiveDrops,
        os::network_statistics_field::TransmitDrops,
        os::network_statistics_field::ReceiveLinkSpeed,
        os::network_statistics_field::TransmitLinkSpeed,
        os::network_statistics_field::TransmitQueueLength,
        os::network_statistics_field::TransmitQueueLimit,
    };
    for (usize index = 0; index < countof(counters); index++) {
      output += "  ";
      let value = String{allocator};
      if (entry.has_field(FIELDS[index])) {
        if (index == 6 || index == 7) {
          value = format_human_size(counters[index], allocator);
          value += "/s";
        } else {
          value = String::from(counters[index], allocator);
        }
      }
      append_report_column(output,
                           entry.has_field(FIELDS[index]) ? value.view()
                                                          : StringView{"-"},
                           WIDTHS[index + 2], true, {}, should_color);
    }
    output += "\n";

    let warning = String{allocator};
    let const do_append_nonzero =
        [&](StringView name, os::network_statistics_field field, u64 value)
            throws -> void {
      if (!entry.has_field(field) || value == 0) return;

      if (!warning.is_empty()) warning += ", ";
      warning += name;
      warning += " ";
      warning += String::from(value, allocator).view();
    };
    do_append_nonzero("receive errors",
                      os::network_statistics_field::ReceiveErrors,
                      entry.receive_error_count);
    do_append_nonzero("transmit errors",
                      os::network_statistics_field::TransmitErrors,
                      entry.transmit_error_count);
    do_append_nonzero("receive drops",
                      os::network_statistics_field::ReceiveDrops,
                      entry.receive_drop_count);
    do_append_nonzero("transmit drops",
                      os::network_statistics_field::TransmitDrops,
                      entry.transmit_drop_count);
    if (!warning.is_empty()) {
      let message = String{allocator, "Interface "};
      message += entry.interface_name.view();
      message += " reports ";
      message += warning.view();
      warnings.push(steal(message));
    }
  }

  return statistics.count();
}

fn append_network_traffic_report(String &output, ArrayList<String> &warnings,
                                 Allocator allocator, bool should_color) throws
    -> usize
{
  let statistics = os::read_network_interface_statistics();
  statistics.sort([](const os::network_interface_statistics_entry &left,
                     const os::network_interface_statistics_entry &right) {
    return left.interface_name < right.interface_name;
  });
  return append_network_traffic_statistics_report(output, warnings, allocator,
                                                  statistics, should_color);
}

fn append_tcp_report(String &output, ArrayList<String> &warnings,
                     Allocator allocator, bool should_color) throws -> bool
{
  os::tcp_statistics statistics{};
  if (!os::read_tcp_statistics(statistics)) return false;

  let body = String{allocator};
  let const do_append_group =
      [&](StringView title, const StringView *names, const u64 *values,
          const os::tcp_statistics_field *fields, usize field_count) throws {
        let value = String{allocator};
        for (usize index = 0; index < field_count; index++) {
          if (!statistics.has_field(fields[index])) continue;
          if (!value.is_empty()) value += ", ";
          value += names[index];
          value += " ";
          value += String::from(values[index], allocator).view();
        }
        if (value.is_empty()) return;
        append_report_inline_field(body, title, value.view(),
                                   colors::ansi::BOLD_CYAN, should_color);
        body += "\n";
      };
  constexpr StringView OPEN_NAMES[] = {"active", "passive"};
  const u64 open_values[] = {statistics.active_open_count,
                             statistics.passive_open_count};
  constexpr os::tcp_statistics_field OPEN_FIELDS[] = {
      os::tcp_statistics_field::ActiveOpens,
      os::tcp_statistics_field::PassiveOpens,
  };
  do_append_group("Opens", OPEN_NAMES, open_values, OPEN_FIELDS,
                  countof(OPEN_NAMES));
  constexpr StringView CONNECTION_NAMES[] = {"established", "failed",
                                             "established resets"};
  const u64 connection_values[] = {statistics.current_established_count,
                                   statistics.attempt_failure_count,
                                   statistics.established_reset_count};
  constexpr os::tcp_statistics_field CONNECTION_FIELDS[] = {
      os::tcp_statistics_field::CurrentEstablished,
      os::tcp_statistics_field::AttemptFailures,
      os::tcp_statistics_field::EstablishedResets,
  };
  do_append_group("Connections", CONNECTION_NAMES, connection_values,
                  CONNECTION_FIELDS, countof(CONNECTION_NAMES));
  constexpr StringView SEGMENT_NAMES[] = {"received", "sent", "retransmitted"};
  const u64 segment_values[] = {statistics.received_segment_count,
                                statistics.sent_segment_count,
                                statistics.retransmitted_segment_count};
  constexpr os::tcp_statistics_field SEGMENT_FIELDS[] = {
      os::tcp_statistics_field::ReceivedSegments,
      os::tcp_statistics_field::SentSegments,
      os::tcp_statistics_field::RetransmittedSegments,
  };
  do_append_group("Segments", SEGMENT_NAMES, segment_values, SEGMENT_FIELDS,
                  countof(SEGMENT_NAMES));
  constexpr StringView FAILURE_NAMES[] = {
      "input errors",         "sent resets",  "connection drops",
      "receive memory drops", "listen drops", "retransmit timeouts"};
  const u64 failure_values[] = {
      statistics.input_error_count,     statistics.sent_reset_count,
      statistics.connection_drop_count, statistics.receive_memory_drop_count,
      statistics.listen_drop_count,     statistics.retransmit_timeout_count};
  constexpr os::tcp_statistics_field FAILURE_FIELDS[] = {
      os::tcp_statistics_field::InputErrors,
      os::tcp_statistics_field::SentResets,
      os::tcp_statistics_field::ConnectionDrops,
      os::tcp_statistics_field::ReceiveMemoryDrops,
      os::tcp_statistics_field::ListenDrops,
      os::tcp_statistics_field::RetransmitTimeouts,
  };
  do_append_group("Failures", FAILURE_NAMES, failure_values, FAILURE_FIELDS,
                  countof(FAILURE_NAMES));
  append_report_body(output, body.view(), "");

  constexpr StringView WARNING_NAMES[] = {
      "failed connections",   "established resets", "retransmitted segments",
      "input errors",         "sent resets",        "connection drops",
      "receive memory drops", "listen drops",       "retransmit timeouts",
  };
  const u64 warning_values[] = {
      statistics.attempt_failure_count,
      statistics.established_reset_count,
      statistics.retransmitted_segment_count,
      statistics.input_error_count,
      statistics.sent_reset_count,
      statistics.connection_drop_count,
      statistics.receive_memory_drop_count,
      statistics.listen_drop_count,
      statistics.retransmit_timeout_count,
  };
  constexpr os::tcp_statistics_field WARNING_FIELDS[] = {
      os::tcp_statistics_field::AttemptFailures,
      os::tcp_statistics_field::EstablishedResets,
      os::tcp_statistics_field::RetransmittedSegments,
      os::tcp_statistics_field::InputErrors,
      os::tcp_statistics_field::SentResets,
      os::tcp_statistics_field::ConnectionDrops,
      os::tcp_statistics_field::ReceiveMemoryDrops,
      os::tcp_statistics_field::ListenDrops,
      os::tcp_statistics_field::RetransmitTimeouts,
  };
  let warning = String{allocator};
  for (usize index = 0; index < countof(WARNING_NAMES); index++) {
    if (!statistics.has_field(WARNING_FIELDS[index]) ||
        warning_values[index] == 0)
    {
      continue;
    }

    if (!warning.is_empty()) warning += ", ";
    warning += WARNING_NAMES[index];
    warning += " ";
    warning += String::from(warning_values[index], allocator).view();
  }
  if (!warning.is_empty()) {
    let message = String{allocator, "TCP counters include "};
    message += warning.view();
    warnings.push(steal(message));
  }

  return true;
}

pure fn network_counter_rate(u64 before, u64 after,
                             u64 elapsed_nanoseconds) wontthrow -> u64
{
  if (after < before || elapsed_nanoseconds == 0) return 0;

  return static_cast<u64>(static_cast<u128>(after - before) * 1000000000ULL /
                          elapsed_nanoseconds);
}

fn sample_network_statistics(
    const ArrayList<os::network_interface_statistics_entry> &before,
    const ArrayList<os::network_interface_statistics_entry> &after,
    u64 elapsed_nanoseconds, Allocator allocator)
    throws -> ArrayList<os::network_interface_statistics_entry>
{
  let sampled = ArrayList<os::network_interface_statistics_entry>{allocator};
  sampled.reserve(after.count());
  for (let const &entry : after) {
    const os::network_interface_statistics_entry *previous = nullptr;
    for (let const &candidate : before) {
      if (candidate.interface_name.view() == entry.interface_name.view()) {
        previous = &candidate;
        break;
      }
    }

    let result = entry;
    result.interface_name = String{allocator, entry.interface_name.view()};
    if (previous != nullptr) {
      if (entry.has_field(os::network_statistics_field::ReceiveBytes) &&
          previous->has_field(os::network_statistics_field::ReceiveBytes))
        result.receive_bytes = network_counter_rate(
            previous->receive_bytes, entry.receive_bytes, elapsed_nanoseconds);
      if (entry.has_field(os::network_statistics_field::TransmitBytes) &&
          previous->has_field(os::network_statistics_field::TransmitBytes))
        result.transmit_bytes = network_counter_rate(
            previous->transmit_bytes, entry.transmit_bytes, elapsed_nanoseconds);
      if (entry.has_field(os::network_statistics_field::ReceivePackets) &&
          previous->has_field(os::network_statistics_field::ReceivePackets))
        result.receive_packet_count = network_counter_rate(
            previous->receive_packet_count, entry.receive_packet_count,
            elapsed_nanoseconds);
      if (entry.has_field(os::network_statistics_field::TransmitPackets) &&
          previous->has_field(os::network_statistics_field::TransmitPackets))
        result.transmit_packet_count = network_counter_rate(
            previous->transmit_packet_count, entry.transmit_packet_count,
            elapsed_nanoseconds);
      if (entry.has_field(os::network_statistics_field::ReceiveErrors) &&
          previous->has_field(os::network_statistics_field::ReceiveErrors))
        result.receive_error_count = network_counter_rate(
            previous->receive_error_count, entry.receive_error_count,
            elapsed_nanoseconds);
      if (entry.has_field(os::network_statistics_field::TransmitErrors) &&
          previous->has_field(os::network_statistics_field::TransmitErrors))
        result.transmit_error_count = network_counter_rate(
            previous->transmit_error_count, entry.transmit_error_count,
            elapsed_nanoseconds);
      if (entry.has_field(os::network_statistics_field::ReceiveDrops) &&
          previous->has_field(os::network_statistics_field::ReceiveDrops))
        result.receive_drop_count = network_counter_rate(
            previous->receive_drop_count, entry.receive_drop_count,
            elapsed_nanoseconds);
      if (entry.has_field(os::network_statistics_field::TransmitDrops) &&
          previous->has_field(os::network_statistics_field::TransmitDrops))
        result.transmit_drop_count = network_counter_rate(
            previous->transmit_drop_count, entry.transmit_drop_count,
            elapsed_nanoseconds);
    }
    sampled.push(steal(result));
  }
  return sampled;
}

struct live_network_row
{
  os::network_interface_statistics_entry statistics;
  u64 last_seen_nanoseconds{0};
};

fn run_live_network_traffic(const ExecContext &ec, Allocator allocator,
                            f64 sample_interval_seconds,
                            f64 refresh_interval_seconds, f64 falloff_seconds,
                            bool should_color) throws -> i32
{
  let retained = ArrayList<live_network_row>{allocator};
  let const falloff_nanoseconds =
      static_cast<u64>(falloff_seconds * 1000000000.0);
  let const refresh_interval_nanoseconds =
      static_cast<u64>(refresh_interval_seconds * 1000000000.0);
  let const sample_interval_nanoseconds =
      static_cast<u64>(sample_interval_seconds * 1000000000.0);
  u64 last_refresh_nanoseconds = os::monotonic_nanos();
  u64 last_sample_nanoseconds = last_refresh_nanoseconds;
  let before = os::read_network_interface_statistics();
  let const is_terminal = colors::stdout_is_a_terminal();
  let const is_alternate = is_terminal && enter_alternate_screen(ec);
  defer
  {
    if (is_alternate) leave_alternate_screen(ec);
  };

  loop
  {
    let const before_wait_nanoseconds = os::monotonic_nanos();
    let const sample_elapsed = before_wait_nanoseconds - last_sample_nanoseconds;
    let const refresh_elapsed = before_wait_nanoseconds - last_refresh_nanoseconds;
    let const until_sample = sample_interval_nanoseconds > sample_elapsed
                                 ? sample_interval_nanoseconds - sample_elapsed
                                 : 0;
    let const until_refresh = refresh_interval_nanoseconds > refresh_elapsed
                                  ? refresh_interval_nanoseconds - refresh_elapsed
                                  : 0;
    let const wait_nanoseconds = until_sample < until_refresh ? until_sample
                                                               : until_refresh;
    os::sleep_for_seconds(static_cast<f64>(wait_nanoseconds) / 1000000000.0);
    if (os::INTERRUPT_REQUESTED != 0) {
      os::INTERRUPT_REQUESTED = 0;
      return 130;
    }

    let const now = os::monotonic_nanos();
    if (now - last_sample_nanoseconds >= sample_interval_nanoseconds) {
      let const current = os::read_network_interface_statistics();
      let const sampled = sample_network_statistics(
          before, current, now - last_sample_nanoseconds, allocator);
      for (let const &entry : sampled) {
        bool is_known = false;
        for (usize index = 0; index < retained.count(); index++) {
          if (retained[index].statistics.interface_name.view() !=
              entry.interface_name.view())
            continue;
          retained[index].statistics = entry;
          retained[index].last_seen_nanoseconds = now;
          is_known = true;
          break;
        }
        if (!is_known) retained.push(live_network_row{entry, now});
      }
      for (usize index = retained.count(); index > 0; index--) {
        let const position = index - 1;
        if (now - retained[position].last_seen_nanoseconds >= falloff_nanoseconds)
          retained.remove(position);
      }
      before = steal(current);
      last_sample_nanoseconds = now;
    }
    if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) {
      continue;
    }
    last_refresh_nanoseconds = now;
    retained.sort([](const live_network_row &left,
                     const live_network_row &right) {
      return left.statistics.interface_name < right.statistics.interface_name;
    });
    let statistics =
        ArrayList<os::network_interface_statistics_entry>{allocator};
    statistics.reserve(retained.count());
    for (let const &row : retained)
      statistics.push(row.statistics);
    let output = String{allocator};
    let warnings = ArrayList<String>{allocator};
    append_network_traffic_statistics_report(output, warnings, allocator,
                                             statistics, should_color, true);
    if (is_terminal)
      output = String{allocator, "\x1b[H\x1b[2J"} + output.view();
    ec.print_to_stdout(output);
    for (let const &warning : warnings)
      show_message(Warning{warning.view()}.to_string());
  }
}

} /* namespace */

EvilNet::EvilNet() = default;

pure fn EvilNet::kind() const wontthrow -> Utility::Kind
{
  return Kind::EvilNet;
}

fn EvilNet::execute(const ExecContext &ec, EvalContext &cxt,
                    const ArrayList<String> &args,
                    const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let operand_locations = ArrayList<SourceLocation>{cxt.scratch_allocator()};
  let const operands =
      PARSE_KOSHKIT_ARGS_WITH_LOCATIONS(args, arg_locations, operand_locations);

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (!operands.is_empty()) {
    KOSHKIT_REPORT_ERROR_AT(operand_locations[0], "unexpected operand",
                            "this utility accepts no operands");
    return 1;
  }

  let const allocator = cxt.scratch_allocator();
  let output = String{allocator};
  let warnings = ArrayList<String>{allocator};
  let const should_color = koshkit_should_color();
  if (FLAG_EVILNET_FAILURES.is_enabled() && FLAG_EVILNET_LIVE.is_enabled()) {
    KOSHKIT_REPORT_ERROR_AT(FLAG_EVILNET_FAILURES.value_location(),
                            "conflicting flags",
                            "--failures cannot be combined with --live");
    return 2;
  }
  f64 live_interval_seconds = 0.5;
  if (FLAG_EVILNET_LIVE.has_value()) {
    let const parsed = parse_koshkit_duration_seconds(
        FLAG_EVILNET_LIVE.value(), FLAG_EVILNET_LIVE.value_location(),
        allocator);
    if (parsed <= 0.0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILNET_LIVE.value_location(),
                              "invalid live interval",
                              "use a positive number of seconds");
      return 1;
    }
    live_interval_seconds = parsed;
  }
  f64 sample_interval_seconds = live_interval_seconds;
  if (FLAG_EVILNET_CUMULATIVE.has_value()) {
    sample_interval_seconds = parse_koshkit_duration_seconds(
        FLAG_EVILNET_CUMULATIVE.value(),
        FLAG_EVILNET_CUMULATIVE.value_location(), allocator);
    if (sample_interval_seconds <= 0.0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILNET_CUMULATIVE.value_location(),
                              "invalid cumulative interval",
                              "use a positive number of seconds");
      return 1;
    }
  }
  if (FLAG_EVILNET_LIVE.is_enabled()) {
    f64 falloff_seconds = 5.0;
    if (FLAG_EVILNET_FALLOFF.is_set()) {
      let const parsed = utils::parse_decimal_f64(FLAG_EVILNET_FALLOFF.value());
      if (parsed.is_error() || parsed.value() <= 0) {
        KOSHKIT_REPORT_ERROR_AT(FLAG_EVILNET_FALLOFF.value_location(),
                                "invalid falloff interval",
                                "use a positive number of seconds");
        return 1;
      }
      falloff_seconds = parsed.value();
    }
    return run_live_network_traffic(ec, allocator, sample_interval_seconds,
                                    live_interval_seconds, falloff_seconds,
                                    should_color);
  }
  let const should_show_all = FLAG_EVILNET_ALL.is_enabled();
  let const should_show_traffic =
      should_show_all || FLAG_EVILNET_TRAFFIC.is_enabled();
  let const should_show_failures = FLAG_EVILNET_FAILURES.is_enabled();
  let const should_show_interfaces =
      !FLAG_EVILNET_TRAFFIC.is_enabled() && !should_show_failures;
  let const address_count = should_show_interfaces
                                ? append_network_interface_report(
                                      output, should_color, "")
                                : 0;
  usize traffic_count = 0;
  bool has_tcp_statistics = false;
  if (should_show_traffic && !should_show_failures) {
    traffic_count = append_network_traffic_report(output, warnings, allocator,
                                                  should_color);
  }
  if (should_show_all || should_show_failures)
    has_tcp_statistics =
        append_tcp_report(output, warnings, allocator, should_color);

  if (!warnings.is_empty()) output += "\n";
  ec.print_to_stdout(output);
  for (let const &warning : warnings) {
    show_message(Warning{warning.view()}.to_string());
  }

  return address_count == 0 && traffic_count == 0 && !has_tcp_statistics ? 1
                                                                         : 0;
}

} /* namespace koshka::koshkit */
