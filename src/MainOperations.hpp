#pragma once

namespace koshka {

fn kosh_binary_flag_list() wontthrow -> const FlagList & { return FLAG_LIST; }

#if !defined NDEBUG
static fn run_debug_completion_driver(StringView driver_line,
                                      EvalContext &context) throws -> i32
{
  context.get_program_resolver().initialize_path_map();
  usize driver_cursor = driver_line.length;
  if (let const cursor_text =
          os::get_environment_variable("KOSH_TEST_COMPLETE_CURSOR");
      cursor_text.has_value())
  {
    let const parsed_cursor = cursor_text->view().to<u64>();
    if (!parsed_cursor.is_error() &&
        parsed_cursor.value() <= driver_line.length)
      driver_cursor = static_cast<usize>(parsed_cursor.value());
  }
  let const driver_result = completion::complete(
      driver_line, driver_cursor, context, Path::current_directory(),
      completion::completion_mode::Listing);
  let listing = String{heap_allocator()};
  for (let const &candidate : driver_result.candidates) {
    listing += candidate.view();
    listing += '\n';
  }
  print(listing);
  flush();
  return 0;
}

static fn run_debug_highlight_driver(StringView driver_line,
                                     EvalContext &context) throws -> i32
{
  let const variable_name_visit_count_before =
      context.debug_variable_name_enumeration_count();
  let const directory_read_count_before = utils::debug_directory_read_count();
  context.get_program_resolver().begin_explicit_completion(
      ProgramResolver::CompletionRefresh::Fresh);
  defer { context.get_program_resolver().end_explicit_completion(); };
  let const spans = completion::highlight_line(driver_line, context);
  let listing = String{heap_allocator()};
  for (let const &span : spans) {
    listing +=
        driver_line.substring_of_length(span.start, span.end - span.start);
    listing += '\t';
    listing += highlight_role_name(span.role);
    listing += '\n';
  }
  LOG(All, "highlighting visited %zu variable names",
      context.debug_variable_name_enumeration_count() -
          variable_name_visit_count_before);
  LOG(All, "highlighting read %zu directories",
      utils::debug_directory_read_count() - directory_read_count_before);
  if (LOGGER_VERBOSITY == verbosity::All)
    LOG(All, "the diagnostic highlight cache is stable %d",
        completion::debug_diagnostic_cache_is_stable(context));
  print(listing);
  flush();
  return 0;
}

static fn run_debug_ghost_driver(StringView driver_line,
                                 EvalContext &context) throws -> i32
{
  let const directory_stat_count_before = utils::debug_directory_stat_count();
  let const directory_read_count_before = utils::debug_directory_read_count();
  context.get_program_resolver().initialize_path_map();
  let const result = completion::complete(driver_line, driver_line.length,
                                          context, Path::current_directory(),
                                          completion::completion_mode::Ghost);
  print("count=" + String::from(result.candidate_count, heap_allocator()) +
        "\nprefix=" + result.longest_common_prefix.view() + "\nsource-scans=" +
        String::from(result.source_candidate_scan_count, heap_allocator()) +
        "\nmaterialized=" +
        String::from(result.materialized_candidate_count, heap_allocator()) +
        "\ndirectory-stats=" +
        String::from(utils::debug_directory_stat_count() -
                         directory_stat_count_before,
                     heap_allocator()) +
        "\ndirectory-reads=" +
        String::from(utils::debug_directory_read_count() -
                         directory_read_count_before,
                     heap_allocator()) +
        "\n");
  flush();
  return 0;
}
#endif /* NDEBUG */

/* The session mood, from --mood when given, then the invocation mood, then the
   strict default. --dumb forces the sh mood when --mood is absent, and --posix
   selects the bash-with-posix-identity mood so a terminal that re-execs with it
   to inject its integration runs as bash. */
pure static fn resolve_session_mood(mimic_mood invocation_mood) wontthrow
    -> mimic_mood
{
  if (FLAG_MOOD.is_set()) {
    if (Maybe<mimic_mood> parsed_mood = parse_mood_name(FLAG_MOOD.value());
        parsed_mood.has_value())
    {
      return *parsed_mood;
    }
    return mimic_mood::Default;
  }
  if (FLAG_DUMB.is_enabled()) return mimic_mood::Posix;
  if (FLAG_POSIX_COMPAT.is_enabled()) return mimic_mood::BashPosix;
  return invocation_mood;
}

static fn
append_listed_diagnostic(String &listing,
                         const diagnostic_definition &definition) throws -> void
{
  let row = String{heap_allocator()};
  if (definition.shellcheck_code.has_value()) {
    char code_text[32];
    row += "SC";
    row += utils::int_to_text_into(*definition.shellcheck_code, code_text,
                                   sizeof(code_text));
    row += ": ";
  }
  row += definition.slug;
  row += " (";
  row += get_diagnostic_tier_name(definition.tier);
  row += "): ";

  let const summary = StringView{definition.summary};
  usize capital_position = 0;
  while (capital_position < summary.length &&
         (summary[capital_position] < 'a' || summary[capital_position] > 'z'))
  {
    capital_position++;
  }
  row += summary.substring_of_length(0, capital_position);
  if (capital_position < summary.length) {
    row += static_cast<char>(summary[capital_position] - 'a' + 'A');
    capital_position++;
  }
  row += summary.substring_of_length(capital_position,
                                     summary.length - capital_position);
  listing +=
      wrap_text(row.view(), HELP_INDENT, HELP_WRAP_WIDTH, HELP_INDENT + 2);
  listing += '\n';
}

static fn print_help_or_version_status(const String &program_path) -> Maybe<int>
{
  if (FLAG_HELP.is_enabled()) {
    let h = String{heap_allocator()};
    h += "KOSHKA";
    h += "\n";
    h += wrap_text("Koshka is a fast and pedantic Bash-compatible command line "
                   "interpreter, formatter, linter, language server and a "
                   "friendly interactive shell.\n\n",
                   HELP_INDENT, HELP_WRAP_WIDTH);
    h += make_synopsis(program_path.view(), HELP_SYNOPSIS);
    h += '\n';
    h += wrap_text("Options are also read from the KOSH_FLAGS environment "
                   "variable. A flag "
                   "on the command line overrides one set there.\n\n",
                   HELP_INDENT, HELP_WRAP_WIDTH);
    h += make_flag_help(FLAG_LIST);
    h += '\n';
    h += '\n';
    h += "Report bugs and suggest features at "
         "<https://github.com/toiletbril/kosh>";
    h += '\n';
    print_error(h);
    return EXIT_SUCCESS;
  }
  if (FLAG_LIST_CHECKS.is_enabled()) {
    let l = String{"SHELLCHECK DIAGNOSTICS\n"};
    for (usize index = 0; index < get_diagnostic_count(); index++) {
      let const &definition = DIAGNOSTIC_DEFINITIONS[index];
      if (!definition.shellcheck_code.has_value()) continue;

      append_listed_diagnostic(l, definition);
    }

    l += "\nNATIVE ANALYSIS DIAGNOSTICS\n";
    for (usize index = 0; index < get_diagnostic_count(); index++) {
      let const &definition = DIAGNOSTIC_DEFINITIONS[index];
      if (definition.shellcheck_code.has_value()) continue;

      append_listed_diagnostic(l, definition);
    }
    print(l);
    return EXIT_SUCCESS;
  }
  if (FLAG_VERSION.is_enabled()) {
    show_version();
    return EXIT_SUCCESS;
  }
  if (FLAG_SHORT_VERSION.is_enabled()) {
    show_short_version();
    return EXIT_SUCCESS;
  }

  return None;
}

static fn report_escaped_control_flow(EvalContext &context,
                                      const String &fallback_source) -> void
{
  if (!context.has_pending_control_flow()) return;

  const control_flow &control = context.pending_control_flow();
  let what = String{heap_allocator()};
  switch (control.kind) {
  case control_flow::Kind::Break:
    what = "'break' used outside of a loop";
    break;
  case control_flow::Kind::Continue:
    what = "'continue' used outside of a loop";
    break;
  case control_flow::Kind::Return: {
    /* A return at the top of a non-interactive script ends the shell with its
       status, the way dash treats a top-level return. */
    if (!context.shell_is_interactive()) {
      i32 return_status = static_cast<i32>(control.value);
      context.clear_control_flow();
      context.run_exit_trap();
      utils::quit(return_status, utils::farewell_policy::Goodbye);
    }
    what = "'return' used outside of a function or a sourced script";
    break;
  }
  case control_flow::Kind::Exit:
  case control_flow::Kind::Normal: context.clear_control_flow(); return;
  }

  const String *source =
      control.source != nullptr ? control.source : &fallback_source;
  let const located = ErrorWithLocation{control.location, what};
  show_message(located.to_string(*source, &context));

  context.clear_control_flow();
}

/* One top-level command at a time for the paths that only lint. The arena is
   rewound to the mark taken before each unit, so a large script costs the
   memory of its widest command and not the memory of its whole syntax tree. */
class StreamedAnalysisUnits final : public AnalysisUnitStream
{
public:
  StreamedAnalysisUnits(Parser &parser, BumpArena &arena,
                        ArrayList<String> &parse_errors, EvalContext &context,
                        ArrayList<source_diagnostic> *diagnostic_sink)
      : m_parser{parser}, m_arena{arena}, m_parse_errors{parse_errors},
        m_context{context}, m_diagnostic_sink{diagnostic_sink}
  {}

  fn next_unit() throws -> const Expression * override
  {
    m_mark = m_arena.mark();

    return m_parser.construct_next_top_level_ast(m_parse_errors, &m_context,
                                                 m_diagnostic_sink);
  }

  fn release_unit() throws -> void override
  {
    m_parser.drop_lexer_peek_cache();
    m_arena.release(m_mark);
  }

private:
  Parser &m_parser;
  BumpArena &m_arena;
  ArrayList<String> &m_parse_errors;
  EvalContext &m_context;
  ArrayList<source_diagnostic> *m_diagnostic_sink;
  BumpArena::Mark m_mark{};
};

static fn run_script_contents(
    const String &script_contents, EvalContext &context, BumpArena &ast_arena,
    Maybe<StringView> filename = None, Expression *precompiled_ast = nullptr,
    Expression **out_ast = nullptr, Maybe<usize> history_event_number = None,
    analysis_diagnostic_totals *diagnostic_totals = nullptr,
    ArrayList<source_diagnostic> *diagnostic_sink = nullptr) -> int
{
  i32 exit_code = EXIT_FAILURE;

  try {
    defer { context.end_command(); };

    /* Function bodies live in the separate function arena, so they survive this
       reset. */
    context.clear_retained_sources();
    ast_arena.reset();
    context.reset_scratch_arena();

    let shellcheck_suppressions =
        ArrayList<shellcheck_suppression>{heap_allocator()};
    let analysis_scope_definitions =
        ArrayList<analysis_scope_definition>{heap_allocator()};
    let shellcheck_directive_spans =
        ArrayList<shellcheck_directive_span>{heap_allocator()};
    let heredoc_terminator_misses =
        ArrayList<heredoc_terminator_miss>{heap_allocator()};

    /* The default mood and noexec run analysis. Compatibility moods require
       enabled warnings. The live context is read so a mood or diagnostic
       switch changes the next command. */
    let const run_analysis =
        precompiled_ast == nullptr &&
        (FLAG_OPTIMIZER_DIAGNOSTICS.is_enabled() ||
         ((context.no_exec() ||
           !(context.is_bash_compatible() || context.is_posix_mode()) ||
           context.warnings_enabled()) &&
          !context.diagnostics_disabled()));

    /* A run that only lints holds one top-level command at a time, so the peak
       memory of a large script is the memory of its widest command. */
    let const should_stream_units =
        run_analysis && precompiled_ast == nullptr && context.no_exec() &&
        out_ast == nullptr && !context.show_ast() &&
        !context.show_lexed_words();

    /* A function body parsed into the function arena would outlive the unit
       that defined it, and that arena is never reset. */
    BumpArena *const previous_function_arena = FUNCTION_ARENA;
    if (should_stream_units) FUNCTION_ARENA = nullptr;
    defer { FUNCTION_ARENA = previous_function_arena; };

    /* A file with any parse error must not run, so every error is collected
       and reported at once. */
    let parse_errors = ArrayList<koshka::String>{heap_allocator()};

    let const do_report_parse_errors = [&]() throws -> bool {
      if (parse_errors.is_empty()) return false;

      if (diagnostic_sink == nullptr)
        for (let const &e : parse_errors)
          show_message(e);
      context.set_last_exit_status(EXIT_FAILURE);

      return true;
    };

    /* A precompiled tree lives in a caller-owned arena that outlives this call.
     */
    Expression *ast = precompiled_ast;
    if (should_stream_units) {
      LOG(Debug, "scanning a chunk of %zu bytes for analysis scopes",
          script_contents.count());

      /* The whole file is scanned first, because analysis resolves a call to a
         function the source defines further down. */
      let scan_parser = Parser{
          Lexer{script_contents.view(), ast_arena, false, filename,
                context.mood()}
      };
      scan_parser.set_should_collect_analysis_scopes(true);

      let const scan_mark = ast_arena.mark();
      loop
      {
        let const unit_mark = ast_arena.mark();
        let const *unit = scan_parser.construct_next_top_level_ast(
            parse_errors, &context, diagnostic_sink);
        if (unit == nullptr) break;

        scan_parser.drop_lexer_peek_cache();
        ast_arena.release(unit_mark);
      }
      ast_arena.release(scan_mark);

      shellcheck_suppressions = scan_parser.take_shellcheck_suppressions();
      analysis_scope_definitions =
          scan_parser.take_analysis_scope_definitions();
      shellcheck_directive_spans =
          scan_parser.take_shellcheck_directive_spans();
      heredoc_terminator_misses = scan_parser.take_heredoc_terminator_misses();

      if (do_report_parse_errors()) return EXIT_FAILURE;
    } else if (precompiled_ast == nullptr) {
      LOG(Debug, "parsing a chunk of %zu bytes", script_contents.count());

      let p = Parser{
          Lexer{script_contents.view(), ast_arena, context.show_lexed_words(),
                filename, context.mood()}
      };
      p.set_should_collect_analysis_scopes(run_analysis);

      ast = p.construct_ast(parse_errors, &context, diagnostic_sink);

      if (do_report_parse_errors()) return EXIT_FAILURE;

      if (context.show_ast()) {
        print(ast->to_ast_string());
        print("\n");
      }

      if (context.show_lexed_words()) {
        for (let const &word : p.debug_words()) {
          print(word.to_pretty_string());
          print("\n");
        }
      }
      shellcheck_suppressions = p.take_shellcheck_suppressions();
      analysis_scope_definitions = p.take_analysis_scope_definitions();
      shellcheck_directive_spans = p.take_shellcheck_directive_spans();
      heredoc_terminator_misses = p.take_heredoc_terminator_misses();
    }

    LOG(Debug, "the analysis stage %s for this chunk",
        run_analysis ? "runs" : "is skipped");
    /* An interactive -W chunk runs right away and the runtime reports a missing
       command itself, so the analysis copy stays quiet to avoid a doubled
       error. */
    bool did_analysis_fail = false;
#if !defined NDEBUG
    let const diagnostic_highlight_bytes_before =
        completion::debug_highlight_input_byte_count();
    let const diagnostic_lexical_scan_bytes_before =
        completion::debug_shell_lexical_scan_byte_count();
#endif
    if (run_analysis) {
      let followed_source_paths = HashSet{heap_allocator()};
      let source_effects_cache =
          StringMap<followed_source_effects>{heap_allocator()};
      if (filename.has_value()) {
        if (let canonical_root = os::canonical_path(Path{*filename});
            canonical_root.has_value())
        {
          followed_source_paths.add(canonical_root->text().view());
        }
      }
      let highlight_cache = completion::shell_highlight_cache{};
      let *previous_highlight_cache =
          context.set_diagnostic_highlight_cache(&highlight_cache);
      defer
      {
        context.set_diagnostic_highlight_cache(previous_highlight_cache);
      };
      let const do_analyze = [&](AnalysisUnitStream *units) throws -> bool {
        return analyze_ast(
            ast, script_contents, context.function_names(),
            context.alias_names(), &context, context.warning_level(),
            context.warnings_enabled() && context.shell_is_interactive(),
            context.mood() == mimic_mood::Default,
            context.annoying_diagnostics_enabled(), shellcheck_suppressions,
            analysis_scope_definitions, shellcheck_directive_spans,
            heredoc_terminator_misses, filename.has_value(),
            FLAG_OPTIMIZER_DIAGNOSTICS.is_enabled(), &followed_source_paths,
            &source_effects_cache, nullptr, diagnostic_totals, true, true,
            nullptr, diagnostic_sink, nullptr, nullptr, units);
      };

      if (should_stream_units) {
        let unit_parser = Parser{
            Lexer{script_contents.view(), ast_arena, false, filename,
                  context.mood()}
        };
        /* A function body and a subshell carry their own definitions on the
           node, and the walk seeds them when it enters. */
        unit_parser.set_should_collect_analysis_scopes(true);

        let units = StreamedAnalysisUnits{unit_parser, ast_arena, parse_errors,
                                          context, diagnostic_sink};
        did_analysis_fail = !do_analyze(&units);
      } else {
        did_analysis_fail = !do_analyze(nullptr);
      }
    }
#if !defined NDEBUG
    LOG(All, "diagnostic highlighting consumed %zu source bytes",
        completion::debug_highlight_input_byte_count() -
            diagnostic_highlight_bytes_before);
    LOG(All, "diagnostic lexical replay consumed %zu source bytes",
        completion::debug_shell_lexical_scan_byte_count() -
            diagnostic_lexical_scan_bytes_before);
#endif
    if (!did_analysis_fail && out_ast != nullptr) {
      *out_ast = ast;
    }

    if (did_analysis_fail) {
      exit_code = EXIT_FAILURE;
    } else if (context.no_exec()) {
      exit_code = EXIT_SUCCESS;
    } else {
      LOG(Debug, "evaluating the chunk");
      let previous_history_event_number =
          context.current_history_event_number();
      context.set_current_history_event_number(steal(history_event_number));
      defer
      {
        context.set_current_history_event_number(
            steal(previous_history_event_number));
      };
      context.set_current_source(&script_contents, "the script");
      let const command_start_nanos = koshka::os::monotonic_nanos();
      exit_code = static_cast<int>(ast->evaluate(context));
      context.set_last_command_duration_nanos(koshka::os::monotonic_nanos() -
                                              command_start_nanos);
      LOG(Debug, "the chunk finished with exit code %d", exit_code);
      /* A signal trapped during the last command has no following node to
         trigger its action, so the pending traps drain here. */
      if (koshka::os::SIGNAL_PENDING) context.run_pending_traps();
      report_escaped_control_flow(context, script_contents);
      /* script_contents is local, so the frame is dropped before it dangles. */
      context.set_current_source(nullptr, "");
    }
    context.set_last_exit_status(static_cast<i32>(exit_code));

    if (context.show_exit_code())
      print("[Code " + String::from(exit_code, heap_allocator()) + "]\n");

    if (context.stats_enabled()) {
      print(context.make_stats_string());
      print("\n");
    }
  } catch (const ErrorWithLocationAndDetails &e) {
    /* An error thrown from a function body was already rendered at the call
       boundary against the file that defined it. */
    if (!e.was_rendered()) {
      show_message(e.to_string(script_contents, &context));
      show_message(e.details_to_string(script_contents, &context));
    }
    exit_code = e.command_status() != 1
                    ? static_cast<i32>(e.command_status())
                    : (context.is_posix_mode() ? 2 : EXIT_FAILURE);
  } catch (const ErrorWithLocation &e) {
    if (!e.was_rendered()) show_message(e.to_string(script_contents, &context));
    exit_code = e.command_status() != 1
                    ? static_cast<i32>(e.command_status())
                    : (context.is_posix_mode() ? 2 : EXIT_FAILURE);
  } catch (const Error &e) {
    show_message(e.to_string());
    exit_code = e.command_status() != 1
                    ? static_cast<i32>(e.command_status())
                    : (context.is_posix_mode() ? 2 : EXIT_FAILURE);
  } catch (const std::exception &e) {
    show_message(
        "Uncaught exception while executing the AST. Aborting the command.");
    show_message("Last system message: '" + os::last_system_error_message() +
                 "'.");
    show_message("Context: '" + String{e.what()} + "'.");
  } catch (...) {
    show_message(
        "Unexpected system explosion while executing the AST. Exiting.");
    show_message("Last system message: " + os::last_system_error_message());
    utils::quit(EXIT_FAILURE);
  }

  return exit_code;
}

static fn run_prompt_command(EvalContext &context, BumpArena &ast_arena) -> void
{
  Maybe<String> command = context.get_variable_value("PROMPT_COMMAND");
  if (!command.has_value() || command->is_empty()) {
    return;
  }

  command->normalize_crlf_line_endings();

  LOG(Info, "running the PROMPT_COMMAND hook, %zu bytes", command->count());

  let const saved_exit_status = context.last_exit_status();
  let const saved_command_duration_nanos =
      context.last_command_duration_nanos();
  context.set_prompt_command_running(true);
  defer { context.set_prompt_command_running(false); };

  let &cached_text = context.get_prompt_command_cached_text();
  let cached_ast = context.get_prompt_command_cached_ast();
  let &prompt_arena = context.get_prompt_command_arena();
  if (cached_ast != nullptr && cached_text.view() == command->view()) {
    run_script_contents(cached_text, context, ast_arena,
                        StringView{"PROMPT_COMMAND"}, cached_ast);
  } else {
    prompt_arena.reset();
    context.set_prompt_command_cached_ast(nullptr);
    cached_text = String{command->view()};
    Expression *parsed_ast = nullptr;
    run_script_contents(cached_text, context, prompt_arena,
                        StringView{"PROMPT_COMMAND"}, nullptr, &parsed_ast);
    context.set_prompt_command_cached_ast(parsed_ast);
  }

  context.set_last_exit_status(saved_exit_status);
  context.set_last_command_duration_nanos(saved_command_duration_nanos);
}

enum class startup_file_requirement : u8
{
  Optional,
  Explicit,
};

static fn source_file(
    const Path &path, EvalContext &context, BumpArena &ast_arena,
    startup_file_requirement requirement = startup_file_requirement::Optional)
    -> bool
{
  Maybe<String> contents = path.read_entire_file();
  if (!contents) {
    let const is_missing = os::last_system_error_is_missing_file();
    let const reason = os::last_system_error_message();
    if (requirement == startup_file_requirement::Explicit && !is_missing)
      show_message(
          Error{"Unable to read startup file '" + path.text() + "': " + reason}
              .to_string());
    LOG(Info, "skipping '%s' because the file is missing or unreadable",
        path.c_str());
    return false;
  }

  LOG(Info, "sourcing '%s', %zu bytes", path.c_str(), contents->count());

  /* run_source parses into the active arena rather than resetting it, since a
     set --init-moods inside a sourced rc reaches here while that rc's tree is
     live and a reset would free the node mid-walk. */
  unused(ast_arena);
  context.run_source(*contents, path.text().view(), return_handling::Consume,
                     /*call_site=*/None, path.text().view());
  return true;
}

static pure fn selected_rcfile() wontthrow -> Maybe<StringView>
{
  if (!FLAG_RCFILE.is_set() && !FLAG_INIT_FILE.is_set()) {
    return None;
  }
  if (!FLAG_RCFILE.is_set() ||
      (FLAG_INIT_FILE.is_set() &&
       FLAG_INIT_FILE.position() > FLAG_RCFILE.position()))
    return FLAG_INIT_FILE.value();
  return FLAG_RCFILE.value();
}

static fn source_custom_rcfile(StringView name, EvalContext &context,
                               BumpArena &ast_arena) throws -> void
{
  let path = String{context.scratch_allocator(), name};
  if (let home_expanded = utils::expand_leading_tilde_path(path.view());
      home_expanded.has_value())
    path = home_expanded.take();
  source_file(Path{path.view()}, context, ast_arena,
              startup_file_requirement::Explicit);
}

static fn source_environment_file(StringView variable_name,
                                  EvalContext &context,
                                  BumpArena &ast_arena) throws -> void
{
  let const value = context.get_variable_value(variable_name);
  if (!value.has_value() || value->is_empty()) {
    return;
  }

  let expanded = context.expand_modifier_word(value->view(), false);
  if (expanded.is_empty()) return;
  if (let home_expanded = utils::expand_leading_tilde_path(expanded.view());
      home_expanded.has_value())
    expanded = home_expanded.take();
  source_file(Path{expanded.view()}, context, ast_arena,
              startup_file_requirement::Explicit);
}

static fn source_home_file(StringView name, EvalContext &context,
                           BumpArena &ast_arena) throws -> void
{
  if (Maybe<Path> home = os::get_home_directory(); home.has_value()) {
    Path path = home->clone();
    path.push_component(name);
    source_file(path, context, ast_arena);
  }
}

/* The dash login files in POSIX order, /etc/profile then ~/.profile. */
static fn source_posix_login_files(EvalContext &context,
                                   BumpArena &ast_arena) throws -> void
{
  LOG(Info, "sourcing the posix login files");
  source_file(Path{"/etc/profile"}, context, ast_arena);
  source_home_file(".profile", context, ast_arena);
}

/* The bash login files in bash order, /etc/profile then the first existing of
   ~/.bash_profile, ~/.bash_login, ~/.profile. */
static fn source_bash_login_files(EvalContext &context,
                                  BumpArena &ast_arena) throws -> void
{
  LOG(Info, "sourcing the bash login files in bash order");
  source_file(Path{"/etc/profile"}, context, ast_arena);
  if (Maybe<Path> home = os::get_home_directory(); home.has_value()) {
    for (let const name : {".bash_profile", ".bash_login", ".profile"}) {
      Path candidate = home->clone();
      candidate.push_component(name);
      if (source_file(candidate, context, ast_arena)) break;
    }
  }
}

/* The system bashrc the way bash compiled with SYS_BASHRC reads it, the Void
   /etc/bash/bashrc or the Debian /etc/bash.bashrc, whichever exists first. */
static fn source_bash_system_rc(EvalContext &context,
                                BumpArena &ast_arena) throws -> void
{
  LOG(Info, "looking for the system bashrc");
  for (let const path : {"/etc/bash/bashrc", "/etc/bash.bashrc"})
    if (source_file(Path{path}, context, ast_arena)) break;
}

/* The default spec and the guard variable are probed so an already-loaded chain
   is not sourced twice. */
static fn ensure_bash_completion_loaded(EvalContext &context,
                                        BumpArena &ast_arena) throws -> void
{
  if (context.default_completion_spec() != nullptr) {
    LOG(Info, "skipping the bash-completion bootstrap because a "
              "default completion spec is already registered");
    return;
  }
  if (context.get_variable_value("BASH_COMPLETION_VERSINFO").has_value()) {
    LOG(Info, "skipping the bash-completion bootstrap because the "
              "rc chain already loaded the script");
    return;
  }
  LOG(Info, "sourcing the stock bash-completion script");
  let bash_completion_runtime = RuntimeState::capture(context);
  bash_completion_runtime.mood = mimic_mood::Bash;
  let const saved_runtime_state =
      context.enter_definition_state(bash_completion_runtime);
  defer
  {
    context.leave_definition_state(saved_runtime_state,
                                   definition_state_exit::RestoreCaller);
  };
  source_file(Path{"/usr/share/bash-completion/bash_completion"}, context,
              ast_arena);
}

fn source_init_moods(EvalContext &context, BumpArena &ast_arena,
                     const ArrayList<mimic_mood> &moods, bool is_login_shell,
                     bool should_be_interactive) throws -> void
{
  /* Each mood sources under its own grammar, so a bash rc parses with the bash
     grammar and a posix profile with the dash grammar. */
  bool did_source_bash_rc = false;
  bool did_source_bash_env = false;
  for (let flavor : moods) {
    /* A mood already on the sourcing stack is skipped, so a set --init-moods
       inside the rc this is sourcing cannot recurse to overflow. */
    if (context.init_mood_sourcing(flavor)) {
      LOG(Info, "skipping the %s mood, its startup files are already sourcing",
          flavor == mimic_mood::Bash        ? "bash"
          : flavor == mimic_mood::Posix     ? "posix"
          : flavor == mimic_mood::BashPosix ? "bash-posix"
                                            : "kosh");
      continue;
    }
    context.set_init_mood_sourcing(flavor, true);
    defer { context.set_init_mood_sourcing(flavor, false); };
    context.set_mood(flavor);
    LOG(Info, "sourcing the startup files for the %s mood",
        flavor == mimic_mood::Bash        ? "bash"
        : flavor == mimic_mood::Posix     ? "posix"
        : flavor == mimic_mood::BashPosix ? "bash-posix"
                                          : "kosh");
    switch (flavor) {
    case mimic_mood::Default:
      /* A --rcfile replaces the kosh rc with the named file. */
      if (is_login_shell) source_posix_login_files(context, ast_arena);
      if (should_be_interactive) {
        if (let const rcfile = selected_rcfile(); rcfile.has_value()) {
          source_custom_rcfile(*rcfile, context, ast_arena);
        } else {
          source_file(Path{"/etc/koshrc"}, context, ast_arena);
          source_home_file(".koshrc", context, ast_arena);
        }
      }
      break;
    case mimic_mood::Posix:
      if (is_login_shell) source_posix_login_files(context, ast_arena);
      if (should_be_interactive &&
          !context.shell_option_state(shell_option_id::Privileged))
      {
        if (Maybe<String> env = context.get_variable_value("ENV");
            env.has_value() && !env->is_empty())
          source_file(Path{env->view()}, context, ast_arena);
      }
      break;
    case mimic_mood::Bash:
    case mimic_mood::BashPosix:
      /* bash runs the system rc first even under --rcfile, so the order mirrors
         that. BashPosix falls through so --posix finds the bash integration. */
      if (is_login_shell) source_bash_login_files(context, ast_arena);
      if (flavor == mimic_mood::Bash &&
          !context.shell_option_state(shell_option_id::Privileged) &&
          !should_be_interactive && !context.startup_finished() &&
          !did_source_bash_env)
      {
        source_environment_file("BASH_ENV", context, ast_arena);
        did_source_bash_env = true;
      }
      if (should_be_interactive && !is_login_shell && !FLAG_NORC.is_enabled()) {
        did_source_bash_rc = true;
        source_bash_system_rc(context, ast_arena);
        if (let const rcfile = selected_rcfile(); rcfile.has_value())
          source_custom_rcfile(*rcfile, context, ast_arena);
        else
          source_home_file(".bashrc", context, ast_arena);
      }
      break;
    }
    if (is_login_shell || should_be_interactive) {
      context.mark_mood_initialized(flavor);
    }
  }

  /* The bash programmable completion loads once after a bash rc sourced, so it
     parses under the bash grammar. */
  if (did_source_bash_rc) {
    LOG(Info, "bootstrapping the bash programmable completion");
    ensure_bash_completion_loaded(context, ast_arena);
  }
}

pure fn quoted_argv_offset_until(int argc, const char *const *argv,
                                 StringView needle) wontthrow -> usize
{
  usize offset = 0;
  for (int a = 0; a < argc; a++) {
    if (needle == StringView{argv[a], std::strlen(argv[a])}) break;
    offset +=
        shell_quoted_arg_length(StringView{argv[a], std::strlen(argv[a])}) + 1;
  }
  return offset;
}

struct apply_file_snapshot
{
  Path operand_path;
  Path target_path;
  os::file_status status;
  String contents;
};

pure fn file_status_matches(const os::file_status &expected,
                            const os::file_status &actual) wontthrow -> bool
{
  if (expected.has_file_identity && actual.has_file_identity &&
      (expected.device_id != actual.device_id ||
       expected.file_id != actual.file_id))
    return false;

  return expected.mode == actual.mode && expected.size == actual.size &&
         expected.modification_time == actual.modification_time &&
         expected.modification_nanoseconds == actual.modification_nanoseconds &&
         expected.change_time == actual.change_time &&
         expected.change_nanoseconds == actual.change_nanoseconds;
}

static fn read_apply_file(const Path &operand_path) throws
    -> Maybe<apply_file_snapshot>
{
  let target_path = os::canonical_path(operand_path);
  if (!target_path.has_value()) {
    show_message("Unable to resolve '" + operand_path.text() +
                 "': " + os::last_system_error_message());
    return None;
  }
  let status = os::file_status{};
  if (!os::stat_path_following(target_path->text().view(), status)) {
    show_message("Unable to inspect '" + operand_path.text() +
                 "': " + os::last_system_error_message());
    return None;
  }
  if (os::file_type_letter(status.mode) != '-') {
    show_message("The '--apply' option requires a regular file: '" +
                 operand_path.text() + "'.");
    return None;
  }
  let contents = target_path->read_entire_file();
  let verified_status = os::file_status{};
  if (!contents.has_value() ||
      !os::stat_path_following(target_path->text().view(), verified_status) ||
      !file_status_matches(status, verified_status))
  {
    show_message("Refusing to read '" + operand_path.text() +
                 "' because it changed while being processed.");
    return None;
  }

  return apply_file_snapshot{operand_path.clone(), target_path.take(), status,
                             contents.take()};
}

static fn replace_file_contents(const apply_file_snapshot &snapshot,
                                StringView replacement) throws -> bool
{
  let replacement_path = os::write_to_named_temp_file(
      snapshot.target_path.parent(), ".kosh_apply", replacement);
  if (!replacement_path.has_value()) {
    show_message("Unable to create a replacement for '" +
                 snapshot.operand_path.text() +
                 "': " + os::last_system_error_message());
    return false;
  }
  defer { unused(os::remove_file(replacement_path->text().view())); };
  if (!os::set_file_mode(replacement_path->text().view(), snapshot.status.mode))
  {
    show_message("Unable to preserve the mode of '" +
                 snapshot.operand_path.text() +
                 "': " + os::last_system_error_message());
    return false;
  }

  let resolved_operand = os::canonical_path(snapshot.operand_path);
  let current_status = os::file_status{};
  let const current_contents = snapshot.target_path.read_entire_file();
  if (!resolved_operand.has_value() ||
      resolved_operand->text() != snapshot.target_path.text() ||
      !current_contents.has_value() ||
      current_contents->view() != snapshot.contents.view() ||
      !os::stat_path_following(snapshot.target_path.text().view(),
                               current_status) ||
      !file_status_matches(snapshot.status, current_status))
  {
    show_message("Refusing to replace '" + snapshot.operand_path.text() +
                 "' because it changed while being processed.");
    return false;
  }
  if (!os::rename_path(replacement_path->text().view(),
                       snapshot.target_path.text().view()))
  {
    show_message("Unable to replace '" + snapshot.operand_path.text() +
                 "': " + os::last_system_error_message());
    return false;
  }

  return true;
}

struct applied_fix_tally
{
  diagnostic_id id;
  error_severity severity;
  usize count;
};

static fn record_applied_fix(ArrayList<applied_fix_tally> &tallies,
                             diagnostic_id id, error_severity severity) throws
    -> void
{
  for (let &tally : tallies) {
    if (tally.id != id || tally.severity != severity) continue;
    tally.count++;

    return;
  }

  tallies.push(applied_fix_tally{id, severity, 1});
}

static fn append_applied_fix_label(String &label, diagnostic_id id) throws
    -> void
{
  let const &definition = get_diagnostic_definition(id);
  label.append(definition.slug);

  if (!definition.shellcheck_code.has_value()) return;

  label.append(" (SC");
  label.append(String::from(*definition.shellcheck_code, heap_allocator()));
  label.push(')');
}

cold static fn
print_applied_fix_summary(ArrayList<applied_fix_tally> &tallies) throws -> void
{
  if (tallies.is_empty()) return;

  tallies.sort([](const applied_fix_tally &left, const applied_fix_tally &right)
                   wontthrow -> bool {
                     if (left.count != right.count)
                       return left.count > right.count;
                     return ENUM(left.id) < ENUM(right.id);
                   });

  usize warning_count = 0;
  usize error_count = 0;
  usize label_width = 0;
  let labels = ArrayList<String>{heap_allocator()};
  for (let const &tally : tallies) {
    if (tally.severity == error_severity::Warning)
      warning_count += tally.count;
    else
      error_count += tally.count;

    let label = String{heap_allocator()};
    append_applied_fix_label(label, tally.id);
    if (label.count() > label_width) label_width = label.count();
    labels.push(steal(label));
  }

  let const wants_color = colors::stderr_wants_color();
  let const warning_color = wants_color ? colors::ansi::YELLOW : StringView{};
  let const error_color =
      wants_color ? colors::ansi::BOLD_BRIGHT_RED : StringView{};
  let const reset = wants_color ? colors::ansi::RESET : StringView{};

  let summary = String{"Fixed "};

  if (warning_count > 0) {
    summary.append(warning_color);
    summary.append(String::from(warning_count, heap_allocator()));
    summary.append(warning_count == 1 ? " warning" : " warnings");
    summary.append(reset);
  }

  if (warning_count > 0 && error_count > 0) summary.append(" and ");

  if (error_count > 0) {
    summary.append(error_color);
    summary.append(String::from(error_count, heap_allocator()));
    summary.append(error_count == 1 ? " error" : " errors");
    summary.append(reset);
  }

  summary.append(".");
  show_message(summary.view());

  for (usize tally_index = 0; tally_index < tallies.count(); tally_index++) {
    let const &tally = tallies[tally_index];
    let const is_warning = tally.severity == error_severity::Warning;

    let line = String{"  "};
    line.append(labels[tally_index].view());
    for (usize padding = labels[tally_index].count(); padding < label_width;
         padding++)
      line.push(' ');
    line.append("  ");
    line.append(is_warning ? warning_color : error_color);
    line.append(String::from(tally.count, heap_allocator()));
    if (is_warning)
      line.append(tally.count == 1 ? " warning" : " warnings");
    else
      line.append(tally.count == 1 ? " error" : " errors");
    line.append(reset);

    show_message(line.view());
  }
}

static fn run_format_operation(const ArrayList<String> &file_names,
                               bool should_apply, bool should_apply_lint_fixes,
                               mimic_mood mood, BumpArena &ast_arena,
                               EvalContext &context) throws -> int
{
  bool did_fail = false;
  usize input_count = file_names.count();
  if (!should_apply && input_count == 0) input_count = 1;

  for (usize input_index = 0; input_index < input_count; input_index++) {
    Maybe<apply_file_snapshot> snapshot;
    let source = String{heap_allocator()};
    if (file_names.is_empty() || file_names[input_index] == "-") {
      source = utils::read_entire_standard_input();
    } else if (should_apply) {
      snapshot = read_apply_file(Path{file_names[input_index].view()});
      if (!snapshot.has_value()) {
        did_fail = true;
        continue;
      }
      source = snapshot->contents.clone();
    } else {
      let const path = Path{file_names[input_index].view()};
      let contents = path.read_entire_file();
      if (!contents.has_value()) {
        show_message("Could not open '" + path.text() +
                     "': " + os::last_system_error_message());
        did_fail = true;
        continue;
      }
      source = contents.take();
    }

    if (should_apply_lint_fixes) {
      let normalized_source = source.clone();
      normalized_source.normalize_crlf_line_endings();
      let diagnostics = ArrayList<source_diagnostic>{heap_allocator()};
      let const source_name = file_names.is_empty()
                                  ? Maybe<StringView>{}
                                  : Maybe<StringView>{file_names[input_index]};
      unused(run_script_contents(normalized_source, context, ast_arena,
                                 source_name, nullptr, nullptr, None, nullptr,
                                 &diagnostics));
      let normalized_fixes = ArrayList<source_fix>{heap_allocator()};

      for (let const &diagnostic : diagnostics) {
        if (!diagnostic.source_name.is_empty() &&
            (!source_name.has_value() ||
             diagnostic.source_name != *source_name))
          continue;
        for (let const &fix : diagnostic.fixes) {
          let edits = ArrayList<source_edit>{heap_allocator()};
          for (let const &edit : fix.edits)
            edits.push(source_edit{edit.start_position, edit.end_position,
                                   edit.expected.clone(),
                                   edit.replacement.clone()});
          normalized_fixes.push(
              source_fix{fix.title.clone(), steal(edits), fix.is_preferred,
                         fix.is_safe_for_fix_all, fix.origin});
        }
      }
      let const fixes = source_fixes_for_original_line_endings(
          source.view(), normalized_fixes);
      let fixed = apply_source_fixes(source.view(), fixes);
      if (!fixed.has_value()) {
        show_message("Unable to apply non-conflicting fixes to the input.");
        did_fail = true;
        continue;
      }
      source = fixed.take();
    }

    let errors = ArrayList<String>{heap_allocator()};
    let formatted = format_shell_source(source.view(), mood, ast_arena, errors);
    if (!formatted.has_value()) {
      for (let const &error : errors)
        show_message(error.view());
      did_fail = true;
      continue;
    }
    if (should_apply) {
      ASSERT(snapshot.has_value());
      if (formatted->view() != source.view() &&
          !replace_file_contents(*snapshot, formatted->view()))
        did_fail = true;
    } else if (colors::stdout_wants_color()) {
      let highlighted = String{heap_allocator()};
      completion::append_highlighted_source(
          highlighted, formatted->view(), context,
          colors::PRINTED_SOURCE_HIGHLIGHT_THEME);
      print(highlighted);
    } else {
      print(formatted->view());
    }
  }

  return did_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

static fn run_lint_apply_operation(const ArrayList<String> &file_names,
                                   bool should_format, EvalContext &context,
                                   BumpArena &ast_arena) throws -> int
{
  bool did_fail = false;
  let final_totals = analysis_diagnostic_totals{};
  let applied_tallies = ArrayList<applied_fix_tally>{heap_allocator()};

  for (let const &file_name : file_names) {
    let const path = Path{file_name.view()};
    let snapshot = read_apply_file(path);
    if (!snapshot.has_value()) {
      did_fail = true;
      continue;
    }
    let source = snapshot->contents.clone();
    source.normalize_crlf_line_endings();
    let diagnostics = ArrayList<source_diagnostic>{heap_allocator()};
    unused(run_script_contents(source, context, ast_arena, file_name.view(),
                               nullptr, nullptr, None, nullptr, &diagnostics));
    let normalized_fixes = ArrayList<source_fix>{heap_allocator()};
    let reported_severities = ArrayList<applied_fix_tally>{heap_allocator()};
    for (let const &diagnostic : diagnostics) {
      if (!diagnostic.source_name.is_empty() &&
          diagnostic.source_name != file_name.view())
        continue;
      if (diagnostic.id.has_value()) {
        record_applied_fix(reported_severities, *diagnostic.id,
                           diagnostic.severity);
      }
      for (let const &fix : diagnostic.fixes) {
        let edits = ArrayList<source_edit>{heap_allocator()};
        for (let const &edit : fix.edits) {
          edits.push(source_edit{edit.start_position, edit.end_position,
                                 edit.expected.clone(),
                                 edit.replacement.clone()});
        }
        normalized_fixes.push(source_fix{fix.title.clone(), steal(edits),
                                         fix.is_preferred,
                                         fix.is_safe_for_fix_all, fix.origin});
      }
    }
    let fixes = source_fixes_for_original_line_endings(
        snapshot->contents.view(), normalized_fixes);
    let applied_origins = ArrayList<diagnostic_id>{heap_allocator()};
    let fixed =
        apply_source_fixes(snapshot->contents.view(), fixes, &applied_origins);
    if (!fixed.has_value()) {
      show_message("Unable to apply non-conflicting fixes to '" + path.text() +
                   "'.");
      did_fail = true;
      continue;
    }
    let final_source = fixed.take();
    if (should_format) {
      let errors = ArrayList<String>{heap_allocator()};
      let formatted = format_shell_source(final_source.view(), context.mood(),
                                          ast_arena, errors);
      if (!formatted.has_value()) {
        for (let const &error : errors)
          show_message(error.view());
        did_fail = true;
        continue;
      }
      final_source = formatted.take();
    }
    let validation_source = final_source.clone();
    validation_source.normalize_crlf_line_endings();
    let validation_diagnostics = ArrayList<source_diagnostic>{heap_allocator()};
    unused(run_script_contents(validation_source, context, ast_arena,
                               file_name.view(), nullptr, nullptr, None,
                               nullptr, &validation_diagnostics));
    bool has_parse_error = false;
    for (let const &diagnostic : validation_diagnostics) {
      if (diagnostic.id.has_value()) continue;
      has_parse_error = true;
      show_message(diagnostic.message.view());
    }
    if (has_parse_error) {
      did_fail = true;
      continue;
    }
    if (final_source.view() != snapshot->contents.view() &&
        !replace_file_contents(*snapshot, final_source.view()))
    {
      did_fail = true;
      continue;
    }

    for (let const applied_id : applied_origins) {
      Maybe<error_severity> severity = None;
      for (let const &reported : reported_severities) {
        if (reported.id != applied_id) continue;
        severity = reported.severity;
        break;
      }

      if (!severity.has_value()) continue;
      record_applied_fix(applied_tallies, applied_id, *severity);
    }

    let analyzed_source = final_source.clone();
    analyzed_source.normalize_crlf_line_endings();
    let const final_status = run_script_contents(
        analyzed_source, context, ast_arena, file_name.view(), nullptr, nullptr,
        None, &final_totals);
    if (final_status != EXIT_SUCCESS) did_fail = true;
  }
  print_applied_fix_summary(applied_tallies);
  print_analysis_diagnostic_summary(final_totals);

  return did_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}

} /* namespace koshka */
