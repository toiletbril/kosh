#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

fn EvalContext::register_completion_spec(StringView command,
                                         completion_spec spec) throws -> void
{
  LOG(Debug,
      "registering a completion spec for '%.*s' with function '%s' and %zu "
      "word-list bytes",
      static_cast<int>(command.length), command.data,
      spec.function_name.c_str(), spec.word_list.length());
  m_completion_specs.set(command, steal(spec));
}

fn EvalContext::register_default_completion_spec(completion_spec spec) throws
    -> void
{
  LOG(Debug, "registering the default completion spec");
  m_default_completion_spec = steal(spec);
}

pure fn EvalContext::default_completion_spec() const wontthrow
    -> const completion_spec *
{
  return m_default_completion_spec.has_value() ? &*m_default_completion_spec
                                               : nullptr;
}

pure fn EvalContext::lookup_completion_spec(StringView command) const wontthrow
    -> const completion_spec *
{
  return m_completion_specs.find(command);
}

fn EvalContext::run_completion_function(StringView function_name,
                                        const ArrayList<String> &words,
                                        usize cword, StringView line,
                                        usize point,
                                        i32 *out_exit_status) throws
    -> ArrayList<String>
{
  const Expression *body =
      has_functions() ? find_function(function_name) : nullptr;
  if (body == nullptr) return ArrayList<String>{heap_allocator()};

  LOG(Info,
      "running the completion function '%.*s' with %zu words, cursor word %zu",
      static_cast<int>(function_name.length), function_name.data, words.count(),
      cword);

  set_completion_function_running(true);
  defer { set_completion_function_running(false); };

  let defining_runtime = RuntimeState::capture(*this);
  if (let const *definition_info = function_definition_info_of(function_name);
      definition_info != nullptr)
    defining_runtime = definition_info->defining_runtime;
  else
    defining_runtime.mood = mimic_mood::Bash;
  let const saved_runtime_state = enter_definition_state(defining_runtime);
  defer
  {
    leave_definition_state(saved_runtime_state,
                           definition_state_exit::RestoreCaller);
  };

  let comp_words = ArrayList<String>{heap_allocator()};
  comp_words.reserve(words.count());
  for (let const &word : words)
    comp_words.push_managed(word.view());
  set_indexed_array("COMP_WORDS", steal(comp_words));
  set_shell_variable("COMP_CWORD",
                     String::from(static_cast<i64>(cword), heap_allocator()));
  set_shell_variable("COMP_LINE", line);
  set_shell_variable("COMP_POINT",
                     String::from(static_cast<i64>(point), heap_allocator()));
  /* bash-completion reassembles the words against COMP_WORDBREAKS, so it is set
     for the function run when a non-bash session left it unset. */
  if (!get_variable_value("COMP_WORDBREAKS").has_value())
    set_shell_variable("COMP_WORDBREAKS", StringView{" \t\n\"'><=;|&(:"});

  /* bash empties COMPREPLY before each completion, so a function that appends
     with COMPREPLY+=() starts clean, and the previous completion on the same
     command does not leak its entries into this one. */
  set_indexed_array("COMPREPLY", ArrayList<String>{heap_allocator()});

  let call_params = ArrayList<String>{heap_allocator()};
  call_params.push(words.is_empty()
                       ? String{heap_allocator()}
                       : String{heap_allocator(), words[0].view()});
  call_params.push(cword < words.count()
                       ? String{heap_allocator(), words[cword].view()}
                       : String{heap_allocator()});
  call_params.push(cword > 0 && cword - 1 < words.count()
                       ? String{heap_allocator(), words[cword - 1].view()}
                       : String{heap_allocator()});

  let saved_params = take_positional_params();
  set_positional_params(steal(call_params));
  defer { set_positional_params(steal(saved_params)); };

  enter_function_call(SourceLocation{});
  defer { leave_function_call(); };
  let const saved_loop_depth = loop_depth();
  set_loop_depth(0);
  defer { set_loop_depth(saved_loop_depth); };
  enter_function_scope();
  push_function_call_name(function_name);
  defer
  {
    pop_function_call_name();
    leave_function_scope();
  };
  let const saved_terminal_exec = terminal_exec_allowed();
  set_terminal_exec_allowed(false);
  defer { set_terminal_exec_allowed(saved_terminal_exec); };

  /* A completion function that errors must not abort the prompt, so any error
     is swallowed and a stray break or return is consumed. */
  try {
    body->evaluate(*this);
  } catch (const ErrorBase &error) {
    LOG(Debug, "completion function '%.*s' threw: %s",
        static_cast<int>(function_name.length), function_name.data,
        error.message().c_str());
  }
  /* The return status is read before the control flow is cleared, so a dynamic
     loader that returns 124 to request a retry is seen by the caller. */
  if (out_exit_status != nullptr) *out_exit_status = last_exit_status();
  if (has_pending_control_flow()) clear_control_flow();

  let result = ArrayList<String>{heap_allocator()};
  if (const ArrayList<String> *reply = lookup_indexed_array("COMPREPLY");
      reply != nullptr)
  {
    result.reserve(reply->count());
    for (let const &entry : *reply)
      result.push_managed(entry.view());
  }
  LOG(Info, "completion function '%.*s' returned %zu candidates with status %d",
      static_cast<int>(function_name.length), function_name.data,
      result.count(),
      out_exit_status != nullptr ? *out_exit_status : last_exit_status());
  return result;
}

} /* namespace koshka */
