/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements the evilnet utility. It reports assigned IPv4 and IPv6
 * interface addresses in deterministic content-derived columns.
 */

#include "../CLI.hpp"
#include "../CLIColors.hpp"
#include "../Arena.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../Path.hpp"
#include "../Platform.hpp"
#include "../Utils.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[-aCtlf] [--sort key]");

HELP_DESCRIPTION_DECL(
    "The evilnet utility reports the addresses assigned to each interface.");

FLAG(EVILNET_ALL, Bool, 'a', "all", "Include interface traffic and TCP data.");
FLAG(EVILNET_TRAFFIC, Bool, 't', "traffic", "Show interface traffic only.");
FLAG(EVILNET_SORT, String, '\0', "sort",
     "Sort traffic by a unique prefix of name, rx, tx, packet, error, or drop "
     "counters.");
static pure fn is_evilnet_sample_duration(koshka::StringView value) wontthrow
    -> bool
{
  return !value.is_empty() &&
         ((value[0] >= '0' && value[0] <= '9') || value[0] == '.');
}
FLAG_OPTIONAL(EVILNET_LIVE, 'l', "live",
              Live,
              "Refresh live traffic every N seconds; the default is 0.5 "
              "seconds. N changes refresh only; sampling remains every 0.5 "
              "seconds.",
              is_evilnet_sample_duration, "seconds");
FLAG_OPTIONAL(EVILNET_CUMULATIVE, 'C', "cumulative",
              Live,
              "Measure traffic over an M-second rolling window; the default is "
              "one second.",
              is_evilnet_sample_duration, "seconds");
FLAG(EVILNET_FAILURES, Bool, 'f', "failures",
     "Show TCP failure and packet-loss telemetry only.");
FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(EvilNet);

namespace koshka::koshkit {

namespace {

enum class evilnet_sort_key : u8
{
  Name,
  ReceiveBytes,
  TransmitBytes,
  ReceivePackets,
  TransmitPackets,
  ReceiveErrors,
  TransmitErrors,
  ReceiveDrops,
  TransmitDrops,
};

struct evilnet_sort_spec
{
  evilnet_sort_key key;
  StringView name;
  os::network_statistics_field field;
  u64 os::network_interface_statistics_entry::*member;
};

static constexpr evilnet_sort_spec EVILNET_SORT_SPECS[] = {
    {evilnet_sort_key::Name, "name", os::network_statistics_field::ReceiveBytes,
     &os::network_interface_statistics_entry::receive_bytes},
    {evilnet_sort_key::ReceiveBytes, "rx",
     os::network_statistics_field::ReceiveBytes,
     &os::network_interface_statistics_entry::receive_bytes},
    {evilnet_sort_key::TransmitBytes, "tx",
     os::network_statistics_field::TransmitBytes,
     &os::network_interface_statistics_entry::transmit_bytes},
    {evilnet_sort_key::ReceivePackets, "rx-packets",
     os::network_statistics_field::ReceivePackets,
     &os::network_interface_statistics_entry::receive_packet_count},
    {evilnet_sort_key::TransmitPackets, "tx-packets",
     os::network_statistics_field::TransmitPackets,
     &os::network_interface_statistics_entry::transmit_packet_count},
    {evilnet_sort_key::ReceiveErrors, "rx-errors",
     os::network_statistics_field::ReceiveErrors,
     &os::network_interface_statistics_entry::receive_error_count},
    {evilnet_sort_key::TransmitErrors, "tx-errors",
     os::network_statistics_field::TransmitErrors,
     &os::network_interface_statistics_entry::transmit_error_count},
    {evilnet_sort_key::ReceiveDrops, "rx-drops",
     os::network_statistics_field::ReceiveDrops,
     &os::network_interface_statistics_entry::receive_drop_count},
    {evilnet_sort_key::TransmitDrops, "tx-drops",
     os::network_statistics_field::TransmitDrops,
     &os::network_interface_statistics_entry::transmit_drop_count},
};

struct evilnet_sort_resolution
{
  Maybe<evilnet_sort_key> key{};
  usize match_count{0};
  String matches{heap_allocator()};
};

fn resolve_evilnet_sort_key(StringView value, Allocator allocator) throws
    -> evilnet_sort_resolution
{
  evilnet_sort_resolution result{};
  result.matches = String{allocator};
  for (let const &spec : EVILNET_SORT_SPECS) {
    if (spec.name == value) {
      result.key = spec.key;
      result.match_count = 1;
      result.matches += spec.name;
      return result;
    }
    if (!spec.name.starts_with(value)) continue;
    if (!result.matches.is_empty()) result.matches += ", ";
    result.matches += spec.name;
    result.key = spec.key;
    result.match_count++;
  }
  return result;
}

fn sort_network_statistics(
    ArrayList<os::network_interface_statistics_entry> &statistics,
    Maybe<evilnet_sort_key> sort_key) throws -> void
{
  let const selected = sort_key.value_or(evilnet_sort_key::Name);
  const evilnet_sort_spec *spec = nullptr;
  for (let const &candidate : EVILNET_SORT_SPECS) {
    if (candidate.key == selected) {
      spec = &candidate;
      break;
    }
  }
  statistics.sort([selected, spec](
                      const os::network_interface_statistics_entry &left,
                      const os::network_interface_statistics_entry &right) {
    if (selected == evilnet_sort_key::Name || spec == nullptr)
      return left.interface_name.view() < right.interface_name.view();
    let const left_available = left.has_field(spec->field);
    let const right_available = right.has_field(spec->field);
    if (left_available != right_available) return left_available;
    if (left_available && left.*(spec->member) != right.*(spec->member))
      return left.*(spec->member) > right.*(spec->member);
    return left.interface_name.view() < right.interface_name.view();
  });
}

static fn default_network_interface(Allocator allocator) throws -> Maybe<String>
{
#if defined __linux__
  let contents = Path{"/proc/net/route"}.read_entire_file();
  if (!contents.has_value()) return None;

  usize position = 0;
  while (position < contents->length()) {
    let const line_start = position;
    while (position < contents->length() && contents->view()[position] != '\n')
      position++;
    let const line = contents->view().substring_of_length(
        line_start, position - line_start);
    if (position < contents->length()) position++;
    if (line.starts_with("Iface")) continue;

    StringView words[4]{};
    usize word_count = 0;
    usize word_position = 0;
    while (word_position < line.length && word_count < countof(words)) {
      while (word_position < line.length &&
             (line[word_position] == ' ' || line[word_position] == '\t'))
        word_position++;
      let const word_start = word_position;
      while (word_position < line.length && line[word_position] != ' ' &&
             line[word_position] != '\t')
        word_position++;
      if (word_position > word_start)
        words[word_count++] = line.substring_of_length(
            word_start, word_position - word_start);
    }
    let const has_up_flag =
        word_count >= 4 && words[3].length >= 4 &&
        (words[3][3] == '1' || words[3][3] == '3' || words[3][3] == '5' ||
         words[3][3] == '7' || words[3][3] == '9' || words[3][3] == 'b' ||
         words[3][3] == 'B' || words[3][3] == 'd' || words[3][3] == 'D' ||
         words[3][3] == 'f' || words[3][3] == 'F');
    if (word_count >= 4 && words[1] == "00000000" && has_up_flag)
      return String{allocator, words[0]};
  }
#else
  unused(allocator);
#endif
  return None;
}

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
    bool should_color, StringView duration_suffix,
    const Maybe<String> &default_interface) throws -> usize
{
  usize name_width = 4;
  for (let const &entry : statistics) {
    if (entry.interface_name.length() > name_width) {
      name_width = entry.interface_name.length();
    }
  }

  append_report_column(output, "NAME", name_width, false,
                       colors::ansi::BOLD_CYAN, should_color);
  let receive_header = String{allocator, "RX"};
  let transmit_header = String{allocator, "TX"};
  let receive_packets_header = String{allocator, "RX PACKETS"};
  let transmit_packets_header = String{allocator, "TX PACKETS"};
  let receive_errors_header = String{allocator, "RX ERRORS"};
  let transmit_errors_header = String{allocator, "TX ERRORS"};
  let receive_drops_header = String{allocator, "RX DROPS"};
  let transmit_drops_header = String{allocator, "TX DROPS"};
  receive_header += duration_suffix;
  transmit_header += duration_suffix;
  receive_packets_header += duration_suffix;
  transmit_packets_header += duration_suffix;
  receive_errors_header += duration_suffix;
  transmit_errors_header += duration_suffix;
  receive_drops_header += duration_suffix;
  transmit_drops_header += duration_suffix;
  const StringView HEADERS[] = {
      receive_header.view(),        transmit_header.view(),
      receive_packets_header.view(), transmit_packets_header.view(),
      receive_errors_header.view(),  transmit_errors_header.view(),
      receive_drops_header.view(),   transmit_drops_header.view(),
      "RX CAP",                     "TX CAP",
      "TX QUEUE",                   "TX LIMIT",
  };
  constexpr usize WIDTHS[] = {9, 9, 12, 12, 10, 10, 9, 9, 9, 9, 10, 10};
  for (usize index = 0; index < countof(HEADERS); index++) {
    output += "   ";
    append_report_column(output, HEADERS[index], WIDTHS[index], true,
                         colors::ansi::BOLD_CYAN, should_color);
  }
  output += "\n";
  for (let const &entry : statistics) {
    let const is_default = default_interface.has_value() &&
                           entry.interface_name.view() ==
                               default_interface->view();
    append_report_column(output, entry.interface_name.view(), name_width, false,
                         is_default ? colors::ansi::BOLD_GREEN
                                    : colors::ansi::GREEN,
                         should_color);
    output += "   ";
    append_report_column(
        output,
        entry.has_field(os::network_statistics_field::ReceiveBytes)
            ? format_human_size(entry.receive_bytes, allocator).view()
            : StringView{"-"},
        9, true, colors::ansi::GREEN, should_color);
    output += "   ";
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
      output += "   ";
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
                                 Allocator allocator, bool should_color,
                                 Maybe<evilnet_sort_key> sort_key) throws
    -> usize
{
  let statistics = os::read_network_interface_statistics();
  sort_network_statistics(statistics, sort_key);
  let const default_interface = default_network_interface(allocator);
  return append_network_traffic_statistics_report(output, warnings, allocator,
                                                  statistics, should_color, {},
                                                  default_interface);
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

pure fn network_counter_delta(u64 before, u64 after) wontthrow -> u64
{
  return after < before ? 0 : after - before;
}

pure fn network_counter_reset(
    const os::network_interface_statistics_entry &before,
    const os::network_interface_statistics_entry &after) wontthrow -> bool
{
  let const did_reset =
      [&](os::network_statistics_field field,
          u64 os::network_interface_statistics_entry::*member) {
        return before.has_field(field) && after.has_field(field) &&
               after.*member < before.*member;
      };
  return did_reset(os::network_statistics_field::ReceiveBytes,
                   &os::network_interface_statistics_entry::receive_bytes) ||
         did_reset(os::network_statistics_field::TransmitBytes,
                   &os::network_interface_statistics_entry::transmit_bytes) ||
         did_reset(
             os::network_statistics_field::ReceivePackets,
             &os::network_interface_statistics_entry::receive_packet_count) ||
         did_reset(
             os::network_statistics_field::TransmitPackets,
             &os::network_interface_statistics_entry::transmit_packet_count) ||
         did_reset(
             os::network_statistics_field::ReceiveErrors,
             &os::network_interface_statistics_entry::receive_error_count) ||
         did_reset(
             os::network_statistics_field::TransmitErrors,
             &os::network_interface_statistics_entry::transmit_error_count) ||
         did_reset(
             os::network_statistics_field::ReceiveDrops,
             &os::network_interface_statistics_entry::receive_drop_count) ||
         did_reset(
             os::network_statistics_field::TransmitDrops,
             &os::network_interface_statistics_entry::transmit_drop_count);
}

fn sample_network_statistics(
    const ArrayList<os::network_interface_statistics_entry> &before,
    const ArrayList<os::network_interface_statistics_entry> &after,
    Allocator allocator)
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
        result.receive_bytes =
            network_counter_delta(previous->receive_bytes, entry.receive_bytes);
      if (entry.has_field(os::network_statistics_field::TransmitBytes) &&
          previous->has_field(os::network_statistics_field::TransmitBytes))
        result.transmit_bytes = network_counter_delta(previous->transmit_bytes,
                                                      entry.transmit_bytes);
      if (entry.has_field(os::network_statistics_field::ReceivePackets) &&
          previous->has_field(os::network_statistics_field::ReceivePackets))
        result.receive_packet_count = network_counter_delta(
            previous->receive_packet_count, entry.receive_packet_count);
      if (entry.has_field(os::network_statistics_field::TransmitPackets) &&
          previous->has_field(os::network_statistics_field::TransmitPackets))
        result.transmit_packet_count = network_counter_delta(
            previous->transmit_packet_count, entry.transmit_packet_count);
      if (entry.has_field(os::network_statistics_field::ReceiveErrors) &&
          previous->has_field(os::network_statistics_field::ReceiveErrors))
        result.receive_error_count = network_counter_delta(
            previous->receive_error_count, entry.receive_error_count);
      if (entry.has_field(os::network_statistics_field::TransmitErrors) &&
          previous->has_field(os::network_statistics_field::TransmitErrors))
        result.transmit_error_count = network_counter_delta(
            previous->transmit_error_count, entry.transmit_error_count);
      if (entry.has_field(os::network_statistics_field::ReceiveDrops) &&
          previous->has_field(os::network_statistics_field::ReceiveDrops))
        result.receive_drop_count = network_counter_delta(
            previous->receive_drop_count, entry.receive_drop_count);
      if (entry.has_field(os::network_statistics_field::TransmitDrops) &&
          previous->has_field(os::network_statistics_field::TransmitDrops))
        result.transmit_drop_count = network_counter_delta(
            previous->transmit_drop_count, entry.transmit_drop_count);
    }
    sampled.push(steal(result));
  }
  return sampled;
}

struct live_network_row
{
  String interface_name{heap_allocator()};
  ArrayList<os::network_interface_statistics_entry> history{heap_allocator()};
  ArrayList<u64> history_nanoseconds{heap_allocator()};
  u64 last_seen_nanoseconds{0};
};

pure fn interpolate_network_counter(u64 before, u64 after,
                                    u64 elapsed_nanoseconds,
                                    u64 passed_nanoseconds) wontthrow -> u64
{
  if (after < before || elapsed_nanoseconds == 0) return after;
  return before + static_cast<u64>(
                      static_cast<u128>(after - before) * passed_nanoseconds /
                      elapsed_nanoseconds);
}

fn get_network_window_status(const live_network_row &row,
                             u64 window_start_nanoseconds,
                             Allocator allocator) throws
    -> os::network_interface_statistics_entry
{
  usize oldest = 0;
  while (oldest + 1 < row.history_nanoseconds.count() &&
         row.history_nanoseconds[oldest + 1] <= window_start_nanoseconds)
    oldest++;

  let before = row.history[oldest];
  u64 before_nanoseconds = row.history_nanoseconds[oldest];
  if (before_nanoseconds < window_start_nanoseconds &&
      oldest + 1 < row.history.count())
  {
    let const &next = row.history[oldest + 1];
    let const elapsed_nanoseconds =
        row.history_nanoseconds[oldest + 1] - before_nanoseconds;
    let const passed_nanoseconds =
        window_start_nanoseconds - before_nanoseconds;
    let const do_interpolate =
        [&](u64 os::network_interface_statistics_entry::*member) {
          before.*member = interpolate_network_counter(
              before.*member, next.*member, elapsed_nanoseconds,
              passed_nanoseconds);
        };
    do_interpolate(&os::network_interface_statistics_entry::receive_bytes);
    do_interpolate(&os::network_interface_statistics_entry::transmit_bytes);
    do_interpolate(
        &os::network_interface_statistics_entry::receive_packet_count);
    do_interpolate(
        &os::network_interface_statistics_entry::transmit_packet_count);
    do_interpolate(
        &os::network_interface_statistics_entry::receive_error_count);
    do_interpolate(
        &os::network_interface_statistics_entry::transmit_error_count);
    do_interpolate(
        &os::network_interface_statistics_entry::receive_drop_count);
    do_interpolate(
        &os::network_interface_statistics_entry::transmit_drop_count);
    before_nanoseconds = window_start_nanoseconds;
  }

  let sampled = row.history.back();
  sampled.interface_name = String{allocator, row.interface_name.view()};
  let const &newest = row.history.back();
  let const do_sample =
      [&](u64 os::network_interface_statistics_entry::*member) {
        sampled.*member = network_counter_delta(before.*member, newest.*member);
      };
  do_sample(&os::network_interface_statistics_entry::receive_bytes);
  do_sample(&os::network_interface_statistics_entry::transmit_bytes);
  do_sample(&os::network_interface_statistics_entry::receive_packet_count);
  do_sample(&os::network_interface_statistics_entry::transmit_packet_count);
  do_sample(&os::network_interface_statistics_entry::receive_error_count);
  do_sample(&os::network_interface_statistics_entry::transmit_error_count);
  do_sample(&os::network_interface_statistics_entry::receive_drop_count);
  do_sample(&os::network_interface_statistics_entry::transmit_drop_count);
  return sampled;
}

fn run_live_network_traffic(const ExecContext &ec, Allocator allocator,
                            f64 window_seconds, f64 sample_interval_seconds,
                            f64 refresh_interval_seconds,
                            bool should_color,
                            Maybe<evilnet_sort_key> sort_key) throws -> i32
{
  let retained = ArrayList<live_network_row>{allocator};
  let const falloff_nanoseconds =
      static_cast<u64>(window_seconds * 1000000000.0);
  let const refresh_interval_nanoseconds =
      static_cast<u64>(refresh_interval_seconds * 1000000000.0);
  let const sample_interval_nanoseconds =
      static_cast<u64>(sample_interval_seconds * 1000000000.0);
  u64 last_refresh_nanoseconds = os::monotonic_nanos();
  u64 last_sample_nanoseconds = last_refresh_nanoseconds;
  let const is_terminal =
      os::is_fd_a_tty(ec.out_fd.value_or(KOSH_STDOUT));
  let const is_alternate = is_terminal && enter_alternate_screen(ec);
  let const is_cursor_hidden = is_terminal && hide_cursor(ec);
  let const sample_label = format_live_duration(window_seconds, allocator);
  let const refresh_label =
      format_live_duration(refresh_interval_seconds, allocator);
  let const default_interface = default_network_interface(allocator);
  let frame_arena = BumpArena{};
  let duration_suffix = String{allocator, "/"};
  duration_suffix += sample_label.view();
  let baseline = os::read_network_interface_statistics();
  for (let const &entry : baseline) {
    live_network_row row{};
    row.interface_name = String{allocator, entry.interface_name.view()};
    row.history.push(entry);
    row.history_nanoseconds.push(last_sample_nanoseconds);
    row.last_seen_nanoseconds = last_sample_nanoseconds;
    retained.push(steal(row));
  }
  defer
  {
    if (is_cursor_hidden) show_cursor(ec);
    if (is_alternate) leave_alternate_screen(ec);
  };

  loop
  {
    let const frame_mark = frame_arena.mark();
    defer { frame_arena.release(frame_mark); };
    let const frame_allocator = bump_allocator(frame_arena);

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
      for (let const &entry : current) {
        bool is_known = false;
        for (usize index = 0; index < retained.count(); index++) {
          if (retained[index].interface_name.view() !=
              entry.interface_name.view())
            continue;
          if (network_counter_reset(retained[index].history.back(), entry)) {
            retained[index].history.clear();
            retained[index].history_nanoseconds.clear();
          }
          retained[index].history.push(entry);
          retained[index].history_nanoseconds.push(now);
          retained[index].last_seen_nanoseconds = now;
          is_known = true;
          break;
        }
        if (!is_known) {
          live_network_row row_entry{};
          row_entry.interface_name =
              String{allocator, entry.interface_name.view()};
          row_entry.history.push(entry);
          row_entry.history_nanoseconds.push(now);
          row_entry.last_seen_nanoseconds = now;
          retained.push(steal(row_entry));
        }
      }
      for (usize index = retained.count(); index > 0; index--) {
        let const position = index - 1;
        if (retained[position].history_nanoseconds.back() != now) {
          retained[position].history.push(retained[position].history.back());
          retained[position].history_nanoseconds.push(now);
        }
        if (now - retained[position].last_seen_nanoseconds >=
            falloff_nanoseconds)
        {
          retained.remove(position);
          continue;
        }
        let const window_start =
            now > falloff_nanoseconds ? now - falloff_nanoseconds : 0;
        while (retained[position].history_nanoseconds.count() > 2 &&
               retained[position].history_nanoseconds[1] <= window_start)
        {
          retained[position].history.remove(0);
          retained[position].history_nanoseconds.remove(0);
        }
      }
      last_sample_nanoseconds = now;
    }
    if (now - last_refresh_nanoseconds < refresh_interval_nanoseconds) {
      continue;
    }
    last_refresh_nanoseconds = now;
    let statistics =
        ArrayList<os::network_interface_statistics_entry>{frame_allocator};
    statistics.reserve(retained.count());
    let const window_start =
        last_sample_nanoseconds > falloff_nanoseconds
            ? last_sample_nanoseconds - falloff_nanoseconds
            : 0;
    for (let const &row : retained) {
      statistics.push(
          get_network_window_status(row, window_start, frame_allocator));
    }
    sort_network_statistics(statistics, sort_key);
    let output = String{frame_allocator};
    let warnings = ArrayList<String>{frame_allocator};
    if (is_terminal) output += "\x1b[H\x1b[2J";
    append_live_controls_bar(output, sample_label.view(), refresh_label.view(),
                             should_color);
    append_network_traffic_statistics_report(output, warnings, frame_allocator,
                                             statistics, should_color,
                                             duration_suffix.view(),
                                             default_interface);
    if (!warnings.is_empty()) output += "\n";
    for (let const &warning : warnings) {
      output += Warning{warning.view()}.to_string().view();
      output += "\n";
    }
    ec.print_to_stdout(output);
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
  Maybe<evilnet_sort_key> sort_key{};
  if (FLAG_EVILNET_SORT.is_set()) {
    let const resolved = resolve_evilnet_sort_key(FLAG_EVILNET_SORT.value(),
                                                  allocator);
    if (resolved.match_count == 0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILNET_SORT.value_location(),
                              "invalid sort key",
                              "use name, rx, tx, rx-packets, tx-packets, "
                              "rx-errors, tx-errors, rx-drops, or tx-drops");
      return 1;
    }
    if (resolved.match_count > 1) {
      let note = String{allocator, "Matches "};
      note += resolved.matches.view();
      note += "; use a longer prefix";
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILNET_SORT.value_location(),
                              "ambiguous sort key", note.view());
      return 1;
    }
    sort_key = resolved.key;
  }
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
  f64 window_seconds = FLAG_EVILNET_LIVE.is_enabled() ? 1.0
                                                       : live_interval_seconds;
  if (FLAG_EVILNET_CUMULATIVE.has_value()) {
    window_seconds = parse_koshkit_duration_seconds(
        FLAG_EVILNET_CUMULATIVE.value(),
        FLAG_EVILNET_CUMULATIVE.value_location(), allocator);
    if (window_seconds <= 0.0) {
      KOSHKIT_REPORT_ERROR_AT(FLAG_EVILNET_CUMULATIVE.value_location(),
                              "invalid cumulative interval",
                              "use a positive number of seconds");
      return 1;
    }
  }
  if (FLAG_EVILNET_LIVE.is_enabled()) {
    return run_live_network_traffic(ec, heap_allocator(), window_seconds,
                                    0.5, live_interval_seconds,
                                    should_color, sort_key);
  }
  let const should_show_all = FLAG_EVILNET_ALL.is_enabled();
  let const should_show_traffic =
      should_show_all || FLAG_EVILNET_TRAFFIC.is_enabled() ||
      sort_key.has_value();
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
    if (FLAG_EVILNET_CUMULATIVE.is_enabled()) {
      let const before = os::read_network_interface_statistics();
      os::sleep_for_seconds(window_seconds);
      if (os::INTERRUPT_REQUESTED != 0) {
        os::INTERRUPT_REQUESTED = 0;
        return 130;
      }
      let const after = os::read_network_interface_statistics();
      let sampled = sample_network_statistics(before, after, allocator);
      sort_network_statistics(sampled, sort_key);
      let const default_interface = default_network_interface(allocator);
      let duration_suffix = String{allocator, "/"};
      duration_suffix += format_live_duration(window_seconds, allocator).view();
      traffic_count = append_network_traffic_statistics_report(
          output, warnings, allocator, sampled, should_color,
          duration_suffix.view(), default_interface);
    } else {
      traffic_count = append_network_traffic_report(output, warnings, allocator,
                                                    should_color, sort_key);
    }
  }
  if (should_show_all || should_show_failures)
    has_tcp_statistics =
        append_tcp_report(output, warnings, allocator, should_color);

  if (!warnings.is_empty()) output += "\n";
  ec.print_to_stdout(output);
  for (let const &warning : warnings) {
    show_warning(warning.view());
  }

  return address_count == 0 && traffic_count == 0 && !has_tcp_statistics ? 1
                                                                         : 0;
}

} /* namespace koshka::koshkit */
