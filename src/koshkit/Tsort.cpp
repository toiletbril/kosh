/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file implements and is responsible for the tsort utility in koshkit.
 * The tsort utility writes a topological ordering.
 */

#include "../Cli.hpp"
#include "../Errors.hpp"
#include "../Eval.hpp"
#include "../Koshkit.hpp"
#include "../StringMap.hpp"

FLAG_LIST_DECL();

HELP_SYNOPSIS_DECL("[file]");

HELP_DESCRIPTION_DECL("The tsort utility writes a topological ordering.");

FLAG(HELP, Bool, '\0', "help", "Display help.");

REGISTER_KOSHKIT_UTIL_FLAGS(Tsort);

namespace koshka::koshkit {

struct tsort_vertex
{
  StringView name;
  ArrayList<usize> outgoing;
  usize incoming_count{0};
  bool is_emitted{false};
};

Tsort::Tsort() = default;

pure fn Tsort::kind() const wontthrow -> Utility::Kind { return Kind::Tsort; }

fn Tsort::execute(const ExecContext &ec, EvalContext &cxt,
                  const ArrayList<String> &args,
                  const ArrayList<SourceLocation> &arg_locations) const throws
    -> i32
{
  let const operands = parse_util_operands(FLAG_LIST, args, &arg_locations);
  defer { reset_flags(FLAG_LIST); };

  KOSHKIT_SHOW_HELP_AND_RETURN(ec, args);

  if (operands.count() > 1) return report_usage_error(ec, cxt, args[0].view());

  let const source = operands.is_empty() ? StringView{"-"} : operands[0].view();
  let const content = read_named_or_stdin(ec, source);
  if (!content.has_value()) {
    report_soft_koshkit_error(ec, cxt,
                              "tsort: cannot read '" +
                                  String{cxt.scratch_allocator(), source} +
                                  "': " + os::last_system_error_message());
    return 1;
  }

  let tokens = ArrayList<StringView>{cxt.scratch_allocator()};
  usize position = 0;

  while (position < content->length()) {
    while (position < content->length() &&
           std::isspace(static_cast<u8>((*content)[position])) != 0)
      position++;
    if (position == content->length()) break;

    let const start = position;
    while (position < content->length() &&
           std::isspace(static_cast<u8>((*content)[position])) == 0)
      position++;
    tokens.push(content->view().substring_of_length(start, position - start));
  }

  if ((tokens.count() & 1u) != 0) {
    report_soft_koshkit_error(ec, cxt,
                              "tsort: input contains an odd number of tokens");
    return 1;
  }

  let vertex_indices = StringMap<usize>{cxt.scratch_allocator()};
  let vertices = ArrayList<tsort_vertex>{cxt.scratch_allocator()};
  let const do_vertex_index = [&](StringView name) throws -> usize {
    if (let const *found = vertex_indices.find(name); found != nullptr)
      return *found;

    let const index = vertices.count();
    vertices.push(tsort_vertex{name, ArrayList<usize>{cxt.scratch_allocator()},
                               0, false});
    vertex_indices.set(name, index);
    return index;
  };

  for (usize token_index = 0; token_index < tokens.count(); token_index += 2) {
    let const from = do_vertex_index(tokens[token_index]);
    let const to = do_vertex_index(tokens[token_index + 1]);
    if (from == to) continue;

    vertices[from].outgoing.push(to);
  }

  let edge_marks = ArrayList<usize>{cxt.scratch_allocator()};
  edge_marks.reserve(vertices.count());
  for (usize index = 0; index < vertices.count(); index++)
    edge_marks.push(0);
  for (usize from = 0; from < vertices.count(); from++) {
    let deduplicated = ArrayList<usize>{cxt.scratch_allocator()};
    deduplicated.reserve(vertices[from].outgoing.count());
    let const edge_mark = from + 1;
    for (let const to : vertices[from].outgoing) {
      if (edge_marks[to] == edge_mark) continue;
      edge_marks[to] = edge_mark;
      deduplicated.push(to);
      vertices[to].incoming_count++;
    }
    vertices[from].outgoing = steal(deduplicated);
  }

  let ready = ArrayList<usize>{cxt.scratch_allocator()};
  for (usize index = 0; index < vertices.count(); index++)
    if (vertices[index].incoming_count == 0) ready.push(index);

  let output = String{cxt.scratch_allocator()};
  usize ready_position = 0;
  usize emitted_count = 0;

  while (ready_position < ready.count()) {
    let const index = ready[ready_position++];
    let &vertex = vertices[index];
    if (vertex.is_emitted) continue;

    vertex.is_emitted = true;
    emitted_count++;
    output += vertex.name;
    output += '\n';

    for (let const next : vertex.outgoing) {
      ASSERT(vertices[next].incoming_count > 0);
      vertices[next].incoming_count--;
      if (vertices[next].incoming_count == 0) ready.push(next);
    }
  }

  i32 status = 0;
  if (emitted_count != vertices.count()) {
    report_soft_koshkit_error(ec, cxt, "tsort: input contains a cycle");
    status = 1;

    for (let &vertex : vertices)
      if (!vertex.is_emitted) {
        output += vertex.name;
        output += '\n';
      }
  }

  ec.print_to_stdout(output);
  return status;
}

} // namespace koshka::koshkit
