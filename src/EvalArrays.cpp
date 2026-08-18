#include "Arena.hpp"
#include "Common.hpp"
#include "Debug.hpp"
#include "Errors.hpp"
#include "Eval.hpp"
#include "Platform.hpp"
#include "StaticStringMap.hpp"
#include "Trace.hpp"
#include "Utils.hpp"

namespace koshka {

static constexpr static_string_entry<EvalContext::CallStackVariable>
    CALL_STACK_VARIABLE_ENTRIES[] = {
        {SSK("BASH_LINENO"), EvalContext::CallStackVariable::LineNumber  },
        {SSK("BASH_SOURCE"), EvalContext::CallStackVariable::SourcePath  },
        {SSK("FUNCNAME"),    EvalContext::CallStackVariable::FunctionName},
};
static constexpr StaticStringMap CALL_STACK_VARIABLES{
    CALL_STACK_VARIABLE_ENTRIES};

static fn sparse_array_key(StringView name, usize index,
                           Allocator allocator) throws -> String
{
  let key = String{allocator, name};
  key.push('\x01');
  char index_text[24];
  key.append(utils::int_to_text_into(static_cast<i64>(index), index_text,
                                     sizeof(index_text)));
  return key;
}

template <class Visit>
static fn for_each_sparse_index(const StringMap<String> &sparse,
                                StringView name, Allocator allocator,
                                Visit do_visit) throws -> void
{
  let const prefix = sparse_array_key(name, 0, allocator);
  let const name_prefix = prefix.view().substring_of_length(0, name.length + 1);
  sparse.for_each([&](StringView key, const String &value) throws {
    if (key.length <= name_prefix.length ||
        key.substring_of_length(0, name_prefix.length) != name_prefix)
    {
      return;
    }
    if (let const parsed = key.substring(name_prefix.length).to<i64>();
        !parsed.is_error() && parsed.value() >= 0)
    {
      do_visit(static_cast<usize>(parsed.value()), value);
    }
  });
}

static fn sparse_array_has_entries(const StringMap<String> &sparse,
                                   StringView name, Allocator allocator) throws
    -> bool
{
  let has_entries = false;
  for_each_sparse_index(sparse, name, allocator,
                        [&](usize index, const String &value) {
                          unused(index);
                          unused(value);
                          has_entries = true;
                        });
  return has_entries;
}

struct sparse_array_entry
{
  usize index;
  String value;
};

/* The entries come back sorted by ascending index, always beyond the dense
   run. */
static fn collect_sparse_array_entries(const StringMap<String> &sparse,
                                       StringView name,
                                       Allocator allocator) throws
    -> ArrayList<sparse_array_entry>
{
  let out = ArrayList<sparse_array_entry>{allocator};
  for_each_sparse_index(sparse, name, allocator,
                        [&](usize index, const String &value) throws {
                          out.push(sparse_array_entry{
                              index, String{allocator, value.view()}
                          });
                        });
  out.sort([](const sparse_array_entry &left, const sparse_array_entry &right) {
    return left.index < right.index;
  });
  return out;
}

fn EvalContext::clear_sparse_array(StringView name) throws -> void
{
  if (!m_sparse_array_names.contains(name)) return;

  /* The erase runs after the scan so the map is not mutated while walked. */
  let indices = ArrayList<usize>{scratch_allocator()};
  for_each_sparse_index(m_sparse_array_values, name, scratch_allocator(),
                        [&](usize index, const String &value) throws {
                          unused(value);
                          indices.push(index);
                        });

  for (const usize index : indices)
    m_sparse_array_values.erase(
        sparse_array_key(name, index, scratch_allocator()).view());
  m_sparse_array_names.remove(name);
}

static fn parse_explicit_array_index(StringView element,
                                     StringView &subscript_out,
                                     StringView &value_out) wontthrow -> bool
{
  if (element.length < 3 || element[0] != '[') {
    return false;
  }
  for (usize i = 1; i + 1 < element.length; i++) {
    if (element[i] == ']' && element[i + 1] == '=') {
      subscript_out = element.substring_of_length(1, i - 1);
      value_out = element.substring(i + 2);
      return true;
    }
  }
  return false;
}

fn EvalContext::assign_indexed_array_elements(StringView name,
                                              const ArrayList<String> &elements,
                                              bool is_append) throws -> void
{
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};

  /* POSIX mode has no arrays, so a bash array literal stands in as an empty
     scalar. */
  if (is_posix_mode()) [[unlikely]] {
    LOG(Debug,
        "posix mode stores the array literal for '%.*s' as an empty scalar",
        static_cast<int>(name.length), name.data);
    set_shell_variable(name, "");
    return;
  }

  if (is_associative_array(name)) {
    if (!is_append) {
      clear_associative_array(name);
      m_associative_names.add(name);
    }

    for (const String &element : elements) {
      StringView subscript;
      StringView value;
      if (parse_explicit_array_index(element.view(), subscript, value))
        set_associative_element(name, subscript, value);
      else
        set_associative_element(name, element.view(), "");
    }
    return;
  }

  usize running_index = 0;
  if (is_append) {
    if (let const *array = lookup_indexed_array(name); array != nullptr)
      running_index = array->count();
    if (m_sparse_array_names.contains(name)) {
      let const sparse = collect_sparse_array_entries(
          m_sparse_array_values, name, scratch_allocator());
      if (!sparse.is_empty()) {
        let const next_after_sparse = sparse[sparse.count() - 1].index + 1;
        if (next_after_sparse > running_index)
          running_index = next_after_sparse;
      }
    }
  } else {
    set_indexed_array(name, ArrayList<String>{heap_allocator()});
  }

  for (const String &element : elements) {
    StringView subscript;
    StringView value;
    let index = running_index;
    if (parse_explicit_array_index(element.view(), subscript, value)) {
      i64 raw_index = evaluate_arithmetic(subscript);
      if (raw_index < 0) raw_index += array_negative_index_base(name);
      if (raw_index < 0)
        throw Error{"Unable to index '" + name +
                    "' because the array subscript is invalid"};
      index = static_cast<usize>(raw_index);
      set_array_element(name, index, value);
    } else {
      set_array_element(name, index, element.view());
    }
    running_index = index + 1;
  }
}

fn EvalContext::set_array_element(StringView name, usize index,
                                  StringView value) throws -> void
{
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};

  /* The dense run holds the contiguous prefix from index zero, and any element
     past its end lives in the sparse map keyed by index, so a gap is not
     padded. A first write promotes an existing scalar to element zero. */
  ArrayList<String> *dense = m_indexed_arrays.find(name);
  if (dense == nullptr) {
    let elements = ArrayList<String>{heap_allocator()};
    if (let const *scalar = m_shell_variables.find(name); scalar != nullptr)
      elements.push(String{heap_allocator(), scalar->view()});
    set_indexed_array(name, steal(elements));
    dense = m_indexed_arrays.find(name);
  }
  m_shell_variables.erase(name);
  ASSERT(dense != nullptr);

  let const dense_count = dense->count();
  if (index < dense_count) {
    (*dense)[index] = String{heap_allocator(), value};
    return;
  }
  if (index == dense_count) {
    /* The write extends the run, so any element now at its end migrates from
       the sparse map into the dense run. */
    dense->push(String{heap_allocator(), value});
    loop
    {
      let const key =
          sparse_array_key(name, dense->count(), scratch_allocator());
      let const *migrated = m_sparse_array_values.find(key.view());
      if (migrated == nullptr) break;
      dense->push(String{heap_allocator(), migrated->view()});
      m_sparse_array_values.erase(key.view());
    }
    if (m_sparse_array_names.contains(name) &&
        !sparse_array_has_entries(m_sparse_array_values, name,
                                  scratch_allocator()))
      m_sparse_array_names.remove(name);
    return;
  }
  LOG(All, "holding element %zu of '%.*s' sparsely past the dense run of %zu",
      index, static_cast<int>(name.length), name.data, dense_count);
  m_sparse_array_names.add(name);
  m_sparse_array_values.set(
      sparse_array_key(name, index, scratch_allocator()).view(), value);
}

/* The name and the key are joined by a byte that does not occur in a name. */
static fn associative_composite_key(StringView name, StringView key,
                                    Allocator allocator) throws -> String
{
  let composite = String{allocator, name};
  composite.push('\x01');
  composite.append(key);
  return composite;
}

fn EvalContext::assign_array_element(StringView name, StringView subscript,
                                     StringView value, bool is_append) throws
    -> void
{
  LOG(All, "assigning the array element '%.*s[%.*s]'",
      static_cast<int>(name.length), name.data,
      static_cast<int>(subscript.length), subscript.data);
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};

  char integer_result[24];
  let const do_integer_element_value = [&](Maybe<String> existing)
                                           throws -> StringView {
    let joined = String{scratch_allocator()};
    if (is_append) {
      if (existing.has_value()) joined.append(existing->view());
      append_integer_expression(joined, value);
    } else {
      joined.append(value);
    }
    return utils::int_to_text_into(
        joined.is_empty() ? 0 : evaluate_arithmetic(joined.view()),
        integer_result, sizeof(integer_result));
  };

  if (is_associative_array(name)) {
    let const key = expand_modifier_word(subscript);
    if (is_integer_variable(name)) [[unlikely]] {
      set_associative_element(
          name, key.view(),
          do_integer_element_value(
              lookup_associative_element(name, key.view())));
      return;
    }
    if (is_append) {
      let const existing = lookup_associative_element(name, key.view());
      let combined = existing.has_value()
                         ? String{scratch_allocator(), existing->view()}
                         : String{scratch_allocator()};
      combined += value;
      set_associative_element(name, key.view(), combined.view());
    } else {
      set_associative_element(name, key.view(), value);
    }
    return;
  }

  i64 index = evaluate_arithmetic(subscript);
  if (index < 0) index += array_negative_index_base(name);
  if (index < 0)
    throw Error{"Unable to index '" + name +
                "' because the array subscript is invalid"};

  let const resolved_index = static_cast<usize>(index);
  let const do_lookup_existing_element = [&]() throws -> Maybe<String> {
    if (let const *array = lookup_indexed_array(name);
        array != nullptr && resolved_index < array->count())
      return String{(*array)[resolved_index].view()};

    if (m_sparse_array_names.contains(name)) {
      let const key =
          sparse_array_key(name, resolved_index, scratch_allocator());
      if (let const *sparse = m_sparse_array_values.find(key.view());
          sparse != nullptr)
        return String{sparse->view()};
    }

    if (resolved_index == 0)
      if (let const *scalar = m_shell_variables.find(name); scalar != nullptr)
        return String{scalar->view()};

    return None;
  };

  if (is_integer_variable(name)) [[unlikely]] {
    let existing = Maybe<String>{};
    if (is_append) existing = do_lookup_existing_element();
    set_array_element(name, resolved_index,
                      do_integer_element_value(steal(existing)));
    return;
  }

  let element = String{scratch_allocator(), value};
  if (is_append) {
    let combined = String{scratch_allocator()};
    if (let const existing = do_lookup_existing_element(); existing.has_value())
      combined = String{existing->view()};
    combined += value;
    element = steal(combined);
  }
  set_array_element(name, resolved_index, element.view());
}

fn EvalContext::declare_associative_array(StringView name) throws -> void
{
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};

  LOG(Debug, "declaring '%.*s' as an associative array",
      static_cast<int>(name.length), name.data);
  let scalar = Maybe<String>{};
  if (let const *stored = m_shell_variables.find(name); stored != nullptr)
    scalar = *stored;
  m_associative_names.add(name);
  m_shell_variables.erase(name);
  if (scalar.has_value()) set_associative_element(name, "0", scalar->view());
}

fn EvalContext::set_associative_element(StringView name, StringView key,
                                        StringView value) throws -> void
{
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};

  if (!is_associative_array(name)) {
    m_associative_names.add(name);
    m_shell_variables.erase(name);
  }
  m_associative_values.set(
      associative_composite_key(name, key, scratch_allocator()).view(), value);
}

fn EvalContext::lookup_associative_element(StringView name,
                                           StringView key) const throws
    -> Maybe<String>
{
  if (let const *value = m_associative_values.find(
          associative_composite_key(name, key, scratch_allocator()).view());
      value != nullptr)
    return *value;
  return None;
}

fn EvalContext::associative_keys(StringView name) const throws
    -> ArrayList<String>
{
  let keys = ArrayList<String>{heap_allocator()};
  const String prefix =
      associative_composite_key(name, "", scratch_allocator());
  m_associative_values.for_each([&](StringView composite, const String &value) {
    unused(value);
    if (composite.starts_with(prefix.view()))
      keys.push_managed(composite.substring(prefix.count()));
  });
  return keys;
}

fn EvalContext::associative_values(StringView name) const throws
    -> ArrayList<String>
{
  let values = ArrayList<String>{heap_allocator()};
  const String prefix =
      associative_composite_key(name, "", scratch_allocator());
  m_associative_values.for_each([&](StringView composite, const String &value) {
    if (composite.starts_with(prefix.view())) values.push_managed(value.view());
  });
  return values;
}

fn EvalContext::clear_associative_array(StringView name) throws -> void
{
  if (!is_associative_array(name)) return;
  /* The composite keys are collected before erasing, since removing entries
     while iterating would be unsafe. */
  const String prefix =
      associative_composite_key(name, "", scratch_allocator());
  let to_erase = ArrayList<String>{heap_allocator()};
  m_associative_values.for_each([&](StringView composite, const String &) {
    if (composite.starts_with(prefix.view())) to_erase.push_managed(composite);
  });
  for (const String &composite : to_erase)
    m_associative_values.erase(composite.view());
  m_associative_names.remove(name);
}

fn EvalContext::unset_array_element(StringView name,
                                    StringView subscript) throws -> void
{
  LOG(All, "unsetting the array element '%.*s[%.*s]'",
      static_cast<int>(name.length), name.data,
      static_cast<int>(subscript.length), subscript.data);
  if (is_readonly(name))
    throw Error{"Unable to unset '" + name + "' because it is read only"};

  if (is_associative_array(name)) {
    let const key = expand_modifier_word(subscript);
    m_associative_values.erase(
        associative_composite_key(name, key.view(), scratch_allocator())
            .view());
    return;
  }

  if (ArrayList<String> *array = m_indexed_arrays.find(name); array != nullptr)
  {
    let const index = evaluate_arithmetic(subscript);
    let const array_count = static_cast<i64>(array->count());
    const i64 resolved =
        index < 0 ? index + array_negative_index_base(name) : index;
    if (resolved < 0) return;
    /* An unset leaves a hole at its index without renumbering the tail. The
       elements after it move to the sparse store under their original
       indices. */
    if (resolved < array_count) {
      if (static_cast<usize>(resolved) + 1 < array->count())
        m_sparse_array_names.add(name);
      for (usize i = static_cast<usize>(resolved) + 1;
           i < static_cast<usize>(array_count); i++)
        m_sparse_array_values.set(
            sparse_array_key(name, i, scratch_allocator()).view(),
            (*array)[i].view());
      while (array->count() > static_cast<usize>(resolved))
        array->remove(array->count() - 1);
    } else {
      m_sparse_array_values.erase(sparse_array_key(name,
                                                   static_cast<usize>(resolved),
                                                   scratch_allocator())
                                      .view());
      if (m_sparse_array_names.contains(name) &&
          !sparse_array_has_entries(m_sparse_array_values, name,
                                    scratch_allocator()))
        m_sparse_array_names.remove(name);
    }
  }
}

fn EvalContext::declare_local(StringView name) throws -> void
{
  if (m_local_scope_depth == 0) return;
  if (is_readonly(name))
    throw Error{"Unable to assign '" + name + "' because it is read only"};
  ASSERT(m_local_scope_depth <= m_local_scopes.count());
  /* One binding per scope, the bash rule. A second local of the same name keeps
     the first's saved caller state, so the scope pop restores the true pre-call
     value and the unset peel finds one entry to consume. */
  if (is_local_in_current_scope(name)) return;
  LOG(All, "declaring '%.*s' local in scope depth %zu",
      static_cast<int>(name.length), name.data, m_local_scope_depth);

  /* Each caller form of the name is saved so the scope pop restores it. A copy
     is taken since the body may overwrite the stored array in place. */
  let previous_array = Maybe<ArrayList<String>>{};
  if (m_indexed_arrays.count() != 0)
    if (let const *array = lookup_indexed_array(name); array != nullptr) {
      let copy = ArrayList<String>{heap_allocator()};
      copy.reserve(array->count());
      for (const String &element : *array)
        copy.push_managed(element.view());
      previous_array = steal(copy);
    }

  let const previous_was_associative = is_associative_array(name);
  let previous_keys = ArrayList<String>{heap_allocator()};
  let previous_values = ArrayList<String>{heap_allocator()};
  if (previous_was_associative) {
    previous_keys = associative_keys(name);
    previous_values = associative_values(name);
  }

  let previous_sparse_indices = ArrayList<usize>{heap_allocator()};
  let previous_sparse_values = ArrayList<String>{heap_allocator()};
  if (m_sparse_array_names.contains(name))
    for (const sparse_array_entry &entry : collect_sparse_array_entries(
             m_sparse_array_values, name, heap_allocator()))
    {
      previous_sparse_indices.push(entry.index);
      previous_sparse_values.push(String{heap_allocator(), entry.value.view()});
    }

  /* A local starts with no attributes, so the integer and read-only marks are
     dropped here and the saved flags put them back when the scope ends. */
  let const previous_was_integer = is_integer_variable(name);
  if (previous_was_integer) unmark_integer(name);

  let const previous_was_readonly = is_readonly(name);
  if (previous_was_readonly) unmark_readonly(name);

  /* The export mark is left in place, so a plain local keeps any inherited
     export until the body reassigns the name. */
  let const previous_was_exported = is_exported(name);

  let previous_value = Maybe<String>{};
  if (let const *scalar = m_shell_variables.find(name); scalar != nullptr)
    previous_value = *scalar;
  else if (previous_array.has_value() && !previous_array->is_empty())
    previous_value = previous_array->front();
  else if (previous_was_exported || variable_requires_dynamic_lookup(name))
    previous_value = get_variable_value(name);

  m_local_scopes[m_local_scope_depth - 1].push(local_binding{
      String{name}, steal(previous_value), steal(previous_array),
      previous_was_associative, steal(previous_keys), steal(previous_values),
      steal(previous_sparse_indices), steal(previous_sparse_values),
      previous_was_integer, previous_was_readonly, previous_was_exported});

  /* The live array forms are cleared so a local array starts empty. The scalar
     value is left in place, so a value-less local keeps the caller's value. */
  m_indexed_arrays.erase(name);
  clear_sparse_array(name);
  clear_associative_array(name);
}

hot fn EvalContext::expand_variable(StringView name) const throws -> String
{
  let value = get_variable_value(name);
  if (value.has_value()) return value.take();

  return String{heap_allocator()};
}

fn EvalContext::array_negative_index_base(StringView name) const throws -> i64
{
  i64 base = 0;
  if (let const *array = m_indexed_arrays.find(name); array != nullptr)
    base = static_cast<i64>(array->count());

  if (m_sparse_array_names.contains(name)) {
    for_each_sparse_index(m_sparse_array_values, name, scratch_allocator(),
                          [&](usize index, const String &value) throws {
                            unused(value);
                            let const past_index =
                                index >= static_cast<usize>(INT64_MAX)
                                    ? INT64_MAX
                                    : static_cast<i64>(index) + 1;
                            if (past_index > base) base = past_index;
                          });
  }

  return base;
}

fn EvalContext::array_element_count(StringView name) const throws -> usize
{
  if (bash_dynamic_variables_enabled()) [[unlikely]] {
    if (let const which = CALL_STACK_VARIABLES.find(name); which.has_value())
      return call_stack_frame_count(*which);
  }

  if (is_associative_array(name)) {
    usize element_count = 0;
    let const prefix = associative_composite_key(name, "", scratch_allocator());
    m_associative_values.for_each(
        [&](StringView composite, const String &value) {
          unused(value);
          if (composite.starts_with(prefix.view())) element_count++;
        });

    return element_count;
  }

  usize element_count = 0;
  if (let const *array = lookup_indexed_array(name); array != nullptr)
    element_count = array->count();

  if (m_sparse_array_names.contains(name)) {
    for_each_sparse_index(m_sparse_array_values, name, scratch_allocator(),
                          [&](usize index, const String &value) throws {
                            unused(index);
                            unused(value);
                            element_count++;
                          });
  }

  if (element_count == 0 && get_variable_value(name).has_value())
    element_count = 1;

  return element_count;
}

fn EvalContext::apply_array_subscript(
    StringView name, StringView subscript,
    const SourceLocation *source_location) throws -> String
{
  if (bash_dynamic_variables_enabled()) [[unlikely]] {
    if (let const which = CALL_STACK_VARIABLES.find(name); which.has_value()) {
      let const depth = call_stack_frame_count(*which);

      if (subscript == "@" || subscript == "*") {
        /* The * form joins with the first IFS byte, the @ form with a space. */
        let separator = ' ';
        let has_separator = true;
        if (subscript == "*") {
          has_separator = !m_field_separators.is_empty();
          if (has_separator) separator = m_field_separators.first_character();
        }

        let out = String{scratch_allocator()};
        for (usize i = 0; i < depth; i++) {
          if (i > 0 && has_separator) {
            out.push(separator);
          }
          out.append(
              call_stack_frame_text(*which, i, scratch_allocator()).view());
        }

        return out;
      }

      let index = evaluate_arithmetic(subscript, source_location);
      if (index < 0) index += static_cast<i64>(depth);

      if (index >= 0 && static_cast<usize>(index) < depth) {
        return call_stack_frame_text(*which, static_cast<usize>(index),
                                     scratch_allocator());
      }

      return String{scratch_allocator()};
    }
  }

  /* The associative values come back in the store's order, which need not match
     bash for more than one key. */
  if (is_associative_array(name)) {
    if (subscript == "@" || subscript == "*") {
      let separator = ' ';
      let has_separator = true;
      if (subscript == "*") {
        has_separator = !m_field_separators.is_empty();
        if (has_separator) separator = m_field_separators.first_character();
      }
      let out = String{scratch_allocator()};
      let const values = associative_values(name);
      for (usize i = 0; i < values.count(); i++) {
        if (i > 0 && has_separator) {
          out.push(separator);
        }
        out.append(values[i].view());
      }
      return out;
    }
    let const key =
        expand_modifier_word(subscript, true, true, source_location);
    let const value = lookup_associative_element(name, key.view());
    if (value.has_value()) return String{heap_allocator(), value->view()};

    return String{heap_allocator()};
  }

  const ArrayList<String> *array = lookup_indexed_array(name);

  /* The single-string return loses the per-element split of a quoted
     "${a[@]}", the same limitation the positional "$@" has. */
  if (subscript == "@" || subscript == "*") {
    if (array == nullptr) return expand_variable(name);
    let separator = ' ';
    let has_separator = true;
    if (subscript == "*") {
      has_separator = !m_field_separators.is_empty();
      if (has_separator) separator = m_field_separators.first_character();
    }
    let out = String{scratch_allocator()};
    for (usize i = 0; i < array->count(); i++) {
      if (i > 0 && has_separator) {
        out.push(separator);
      }
      out.append((*array)[i].view());
    }
    return out;
  }

  i64 index = evaluate_arithmetic(subscript, source_location);
  if (array == nullptr) {
    /* A scalar reads as a one-element array, so ${name[0]} is the value and any
       other index is empty. */
    if (index == 0) return expand_variable(name);
    return String{scratch_allocator()};
  }
  let const array_count = static_cast<i64>(array->count());
  if (index < 0) index += array_negative_index_base(name);
  if (index < 0 || index >= array_count) {
    if (index >= 0) {
      let const probe = sparse_array_key(name, static_cast<usize>(index),
                                         scratch_allocator());
      if (let const *sparse = m_sparse_array_values.find(probe.view());
          sparse != nullptr)
      {
        return String{scratch_allocator(), sparse->view()};
      }
    }
    return String{scratch_allocator()};
  }
  return String{scratch_allocator(),
                (*array)[static_cast<usize>(index)].view()};
}

fn EvalContext::collect_array_elements(StringView name) const throws
    -> ArrayList<String>
{
  if (bash_dynamic_variables_enabled()) [[unlikely]] {
    if (let const which = CALL_STACK_VARIABLES.find(name); which.has_value()) {
      let const depth = call_stack_frame_count(*which);

      if (depth > 0) {
        let frames = ArrayList<String>{heap_allocator()};
        frames.reserve(depth);
        for (usize i = 0; i < depth; i++)
          frames.push(call_stack_frame_text(*which, i, heap_allocator()));

        return frames;
      }
    }
  }

  if (is_associative_array(name)) return associative_values(name);

  let out = ArrayList<String>{heap_allocator()};
  if (const ArrayList<String> *array = lookup_indexed_array(name);
      array != nullptr)
  {
    out.reserve(array->count());
    for (const String &element : *array)
      out.push_managed(element.view());
    if (m_sparse_array_names.contains(name)) {
      let sparse = collect_sparse_array_entries(m_sparse_array_values, name,
                                                scratch_allocator());
      for (sparse_array_entry &entry : sparse)
        out.push(steal(entry.value));
    }
    return out;
  }
  if (Maybe<String> scalar = get_variable_value(name); scalar.has_value())
    out.push(steal(*scalar));
  return out;
}

fn EvalContext::array_element_is_set(StringView name,
                                     StringView subscript) throws -> bool
{
  if (subscript == "@" || subscript == "*") {
    return array_element_count(name) != 0;
  }
  if (is_associative_array(name)) {
    let const key = expand_modifier_word(subscript);
    return lookup_associative_element(name, key.view()).has_value();
  }
  let const index = evaluate_arithmetic(subscript);
  if (const ArrayList<String> *array = lookup_indexed_array(name);
      array != nullptr)
  {
    let const array_count = static_cast<i64>(array->count());
    /* A negative index counts from the highest set index, so [[ -v a[-1] ]]
       names the element ${a[-1]} reads. */
    const i64 resolved =
        index < 0 ? index + array_negative_index_base(name) : index;
    if (resolved >= 0 && resolved < array_count) {
      return true;
    }
    return resolved >= 0 &&
           m_sparse_array_values.find(
               sparse_array_key(name, static_cast<usize>(resolved),
                                scratch_allocator())
                   .view()) != nullptr;
  }
  return index == 0 && get_variable_value(name).has_value();
}

fn EvalContext::matching_prefix_names(StringView prefix) const throws
    -> ArrayList<String>
{
  LOG(All, "listing variable names with the prefix '%.*s'",
      static_cast<int>(prefix.length), prefix.data);
  let names = ArrayList<String>{heap_allocator()};
  let seen = HashSet{heap_allocator()};
  let const do_consider = [&](StringView candidate) throws {
    if (candidate.starts_with(prefix) && seen.add(candidate)) {
      names.push_managed(candidate);
    }
  };
  let const stored_names = variable_names();
  stored_names.for_each(do_consider);
  for (const String &environment_name : os::environment_names())
    do_consider(environment_name.view());
  let dynamic_names = ArrayList<StringView>{heap_allocator()};
  append_dynamic_variable_names(dynamic_names);
  for (let const dynamic_name : dynamic_names)
    do_consider(dynamic_name);
  names.sort();
  return names;
}

fn EvalContext::collect_array_subscripts(StringView name) const throws
    -> ArrayList<String>
{
  let out = ArrayList<String>{heap_allocator()};
  if (is_associative_array(name)) {
    let const keys = associative_keys(name);
    out.reserve(keys.count());
    for (const String &key : keys)
      out.push(String{heap_allocator(), key.view()});
    return out;
  }
  if (let const *array = lookup_indexed_array(name); array != nullptr) {
    out.reserve(array->count());
    for (usize i = 0; i < array->count(); i++)
      out.push(String::from(i, heap_allocator()));
    if (m_sparse_array_names.contains(name)) {
      for (const sparse_array_entry &entry : collect_sparse_array_entries(
               m_sparse_array_values, name, scratch_allocator()))
        out.push(String::from(entry.index, heap_allocator()));
    }
    return out;
  }
  if (get_variable_value(name).has_value())
    out.push(String{heap_allocator(), "0"});
  return out;
}

} /* namespace koshka */
