/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file connects evaluator-owned completion specifications with
 * completion functions, their Bash argument frames, and COMPREPLY execution.
 * The split exists because completion evaluation needs completion types and
 * callbacks that the evaluator core does not otherwise include.
 */

#include "Errors.hpp"
#include "Eval.hpp"
#include "Expressions.hpp"
#include "Utils.hpp"
#include "base/Debug.hpp"
#include "base/Trace.hpp"

namespace koshka {

fn EvalContext::register_completion_spec(StringView command,
                                         completion_spec spec) throws -> void
{
  LOG(Debug,
      "registering a completion spec for '%.*s' with function '%s' and %zu "
      "word-list bytes",
      static_cast<int>(command.length), command.data,
      spec.function_name.c_str(), spec.word_list.length());
  completion_store().register_spec(command, steal(spec));
}

fn EvalContext::register_default_completion_spec(completion_spec spec) throws
    -> void
{
  LOG(Debug, "registering the default completion spec");
  completion_store().register_default_spec(steal(spec));
}

pure fn EvalContext::default_completion_spec() const wontthrow
    -> const completion_spec *
{
  return completion_store().default_spec_ptr();
}

pure fn EvalContext::lookup_completion_spec(StringView command) const wontthrow
    -> const completion_spec *
{
  return completion_store().lookup_spec(command);
}

fn EvalContext::run_completion_function(StringView function_name,
                                        const ArrayList<String> &words,
                                        usize cword, StringView line,
                                        usize point,
                                        i32 *out_exit_status) throws
    -> ArrayList<String>
{
  FunctionBodyHandle body_storage{};
  if (has_functions()) {
    if (let const *storage = find_function_storage(function_name);
        storage != nullptr)
    {
      body_storage = *storage;
    }
  }
  const Expression *body =
      body_storage.has_value() ? body_storage.get_body() : nullptr;
  if (body == nullptr) return ArrayList<String>{heap_allocator()};

  LOG(Info,
      "running the completion function '%.*s' with %zu words, cursor word %zu",
      static_cast<int>(function_name.length), function_name.data, words.count(),
      cword);

  set_completion_function_running(true);
  defer { set_completion_function_running(false); };

  let defining_runtime = RuntimeState::capture(*this);
  if (let const *definition_info = body_storage.get_definition_info();
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

  let const do_reset_array = [&](StringView name)
                                 throws -> ArrayList<String> & {
    if (is_readonly(name)) {
      throw Error{"Unable to assign '" + name + "' because it is read only"};
    }

    m_variable_store.shell_variables().erase(name);
    clear_sparse_array(name);
    let &storage = indexed_arrays().get_or_create(
        name, ArrayList<String>{heap_allocator()});
    storage.clear();

    return storage;
  };

  let &comp_words = do_reset_array("COMP_WORDS");
  comp_words.reserve(words.count());
  for (let const &word : words)
    comp_words.push_managed(word.view());

  char number_buffer[32];
  set_shell_variable("COMP_CWORD", utils::int_to_text_into(
                                       static_cast<i64>(cword), number_buffer,
                                       sizeof(number_buffer)));
  set_shell_variable("COMP_LINE", line);
  set_shell_variable("COMP_POINT", utils::int_to_text_into(
                                       static_cast<i64>(point), number_buffer,
                                       sizeof(number_buffer)));
  if (!has_variable_name("COMP_WORDBREAKS")) {
    set_shell_variable("COMP_WORDBREAKS", StringView{" \t\n\"'><=;|&(:"});
  }

  do_reset_array("COMPREPLY");

  let call_params = ArrayList<String>{heap_allocator()};
  call_params.reserve(3);
  call_params.push(words.is_empty()
                       ? String{heap_allocator()}
                       : String{heap_allocator(), words[0].view()});
  call_params.push(cword < words.count()
                       ? String{heap_allocator(), words[cword].view()}
                       : String{heap_allocator()});
  call_params.push(cword > 0 && cword - 1 < words.count()
                       ? String{heap_allocator(), words[cword - 1].view()}
                       : String{heap_allocator()});

  let bash_argument_frame_context = BashArgumentFrameContext{};
  enter_bash_function_argument_frame(bash_argument_frame_context, call_params);
  defer { leave_bash_argument_frame(bash_argument_frame_context); };
  let saved_params = take_positional_params();
  set_positional_params(steal(call_params));
  defer { set_positional_params(steal(saved_params)); };

  enter_function_call(SourceLocation{});
  defer { leave_function_call(); };
  let const saved_loop_depth = loop_depth();
  set_loop_depth(0);
  defer { set_loop_depth(saved_loop_depth); };
  enter_function_scope();
  push_function_call_name(function_name, body_storage);
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
  let was_interrupted = false;
  try {
    body->evaluate(*this);
  } catch (const InterruptErrorWithLocation &) {
    /* The throw already consumed the interrupt request on its way out, so it is
       raised again for the caller. Without it the editor would see a settled
       flag and offer whatever the function had filled in before the user asked
       it to stop. */
    was_interrupted = true;
    os::INTERRUPT_REQUESTED = 1;
    LOG(Debug, "completion function '%.*s' was interrupted",
        static_cast<int>(function_name.length), function_name.data);
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
  if (ArrayList<String> *reply = indexed_arrays().find("COMPREPLY");
      !was_interrupted && reply != nullptr)
  {
    result = steal(*reply);
  }
  LOG(Info, "completion function '%.*s' returned %zu candidates with status %d",
      static_cast<int>(function_name.length), function_name.data,
      result.count(),
      out_exit_status != nullptr ? *out_exit_status : last_exit_status());
  return result;
}

} /* namespace koshka */
