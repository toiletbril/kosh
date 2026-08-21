#include "CompletionInternal.hpp"
#include "LanguageServerProtocol.hpp"
#include "ShellVariables.hpp"

namespace koshka::language_server {

namespace {

struct positioned_document
{
  Document *document;
  protocol_position position;
};

struct positioned_symbol
{
  Document *document;
  document_symbol symbol;
};

enum class request_method : u8
{
  Initialize,
  Initialized,
  Shutdown,
  Exit,
  DidOpen,
  DidChange,
  DidClose,
  Completion,
  ResolveCompletion,
  CodeAction,
  Definition,
  Hover,
  SemanticTokens,
  DocumentSymbols,
  PrepareRename,
  Rename,
};

constexpr static_string_entry<request_method> REQUEST_METHOD_ENTRIES[] = {
    {SSK("initialize"),                       request_method::Initialize       },
    {SSK("initialized"),                      request_method::Initialized      },
    {SSK("shutdown"),                         request_method::Shutdown         },
    {SSK("exit"),                             request_method::Exit             },
    {SSK("textDocument/didOpen"),             request_method::DidOpen          },
    {SSK("textDocument/didChange"),           request_method::DidChange        },
    {SSK("textDocument/didClose"),            request_method::DidClose         },
    {SSK("textDocument/completion"),          request_method::Completion       },
    {SSK("completionItem/resolve"),           request_method::ResolveCompletion},
    {SSK("textDocument/codeAction"),          request_method::CodeAction       },
    {SSK("textDocument/definition"),          request_method::Definition       },
    {SSK("textDocument/hover"),               request_method::Hover            },
    {SSK("textDocument/semanticTokens/full"), request_method::SemanticTokens   },
    {SSK("textDocument/documentSymbol"),      request_method::DocumentSymbols  },
    {SSK("textDocument/prepareRename"),       request_method::PrepareRename    },
    {SSK("textDocument/rename"),              request_method::Rename           },
};
constexpr StaticStringMap REQUEST_METHODS{REQUEST_METHOD_ENTRIES};

enum class rename_kind : u8
{
  variable,
  command,
};

class Server : public AnalysisSourceProvider
{
public:
  Server(EvalContext &context, BumpArena &ast_arena)
      : m_context(context), m_ast_arena(ast_arena),
        m_documents(heap_allocator()),
        m_workspace_root(Path::current_directory())
  {}

  fn run() throws -> int;
  fn read_source(const Path &canonical_path) throws -> Maybe<String> override;

private:
  fn dispatch(const JsonValue &message) throws -> bool;
  fn initialize(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn open_document(const JsonValue *params) throws -> Document *;
  fn change_document(const JsonValue *params) throws -> Document *;
  fn close_document(const JsonValue *params) throws -> void;
  fn complete(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn resolve_completion(const JsonValue *id, const JsonValue *params) throws
      -> bool;
  fn definition(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn hover(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn semantic_tokens(const JsonValue *id, const JsonValue *params) throws
      -> bool;
  fn document_symbols(const JsonValue *id, const JsonValue *params) throws
      -> bool;
  fn prepare_rename(const JsonValue *id, const JsonValue *params) throws
      -> bool;
  fn rename(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn rename_kind_of(const document_symbol &symbol) throws -> Maybe<rename_kind>;
  fn collect_rename_spans(const Document &document, rename_kind kind,
                          StringView name,
                          ArrayList<rename_span> &out_spans) throws -> void;
  fn code_actions(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn publish_diagnostics(Document &document) throws -> bool;
  fn send_diagnostics(const Document &document,
                      const ArrayList<source_diagnostic> &diagnostics) throws
      -> bool;
  fn publish_auxiliary_diagnostics() throws -> bool;
  fn send_empty_diagnostics(StringView uri) throws -> bool;
  fn validate_all(Document *changed_document = nullptr) throws -> bool;
  pure fn find_document(StringView uri) wontthrow -> Document *;
  pure fn request_document(const JsonValue *params) wontthrow -> Document *;
  pure fn request_positioned_document(const JsonValue *params) throws
      -> Maybe<positioned_document>;
  fn request_positioned_symbol(const JsonValue *params) throws
      -> Maybe<positioned_symbol>;
  fn append_diagnostic(String &output, const Document &document,
                       const source_diagnostic &diagnostic,
                       usize diagnostic_index, bool &is_first) throws -> void;
  fn select_document_mood(const Document &document) wontthrow -> void;
  fn symbol_at(const Document &document, protocol_position position) throws
      -> Maybe<document_symbol>;
  fn definition_of(const Document &document,
                   const document_symbol &symbol) throws
      -> Maybe<document_symbol>;
  fn command_information(StringView command) throws -> Maybe<String>;
  fn variable_hover_text(
      const Document &document,
      const ArrayList<const variable_assignment_record *> &reaching) throws
      -> String;
  fn append_shell_variable_facts(
      String &output, const shell_variable_description &description) throws
      -> void;
  fn shell_variable_hover_text(
      StringView name, const shell_variable_description &description) throws
      -> String;
  fn function_hover_text(const Document &document,
                         const function_body_record &record) throws -> String;
  fn send_hover(const JsonValue *id, const Document &document,
                const document_symbol &symbol, StringView value) throws -> bool;
  fn append_text_edit(String &output, const Document &document,
                      usize start_position, usize end_position,
                      StringView replacement) throws -> void;
  fn open_workspace_edit(String &output, const Document &document) throws
      -> void;
  fn close_workspace_edit(String &output) throws -> void;

  EvalContext &m_context;
  BumpArena &m_ast_arena;
  ArrayList<Document> m_documents;
  ArrayList<source_diagnostic> m_current_auxiliary_diagnostics{
      heap_allocator()};
  ArrayList<String> m_published_auxiliary_uris{heap_allocator()};
  ArrayList<String> m_current_auxiliary_uris{heap_allocator()};
  completion::shell_highlight_cache m_highlight_cache;
  Path m_workspace_root;
  position_encoding m_encoding{position_encoding::Utf16};
  bool m_is_initialized{false};
  bool m_is_shutdown{false};
  bool m_supports_document_changes{false};
  bool m_supports_code_action_literals{false};
  bool m_supports_quick_fixes{false};
  bool m_supports_fix_all{false};
  bool m_supports_diagnostic_data{false};
  bool m_supports_preferred_actions{false};
  bool m_supports_markdown_hover{false};
};

static constexpr u32 SEMANTIC_DECLARATION = 1u << 0;
static constexpr u32 SEMANTIC_UNRESOLVED = 1u << 3;
static constexpr u32 SEMANTIC_SET = 1u << 9;
static constexpr u32 SEMANTIC_UNUSED = 1u << 10;

pure fn semantic_style(highlight_role role) wontthrow -> std::pair<u32, u32>
{
  static constexpr u32 READONLY = 1u << 1;
  static constexpr u32 INVALID = 1u << 2;
  static constexpr u32 PARTIAL = 1u << 4;
  static constexpr u32 PATH = 1u << 5;
  static constexpr u32 COMMAND = 1u << 6;
  static constexpr u32 HEREDOC = 1u << 7;
  static constexpr u32 URL = 1u << 8;

  static constexpr std::pair<u32, u32> STYLES[] = {
      {0, 0                            },
      {1, 0                            },
      {2, 0                            },
      {2, HEREDOC                      },
      {8, HEREDOC                      },
      {3, 0                            },
      {3, SEMANTIC_DECLARATION         },
      {3, SEMANTIC_UNRESOLVED          },
      {4, READONLY                     },
      {5, 0                            },
      {1, INVALID                      },
      {6, SEMANTIC_DECLARATION         },
      {6, COMMAND                      },
      {6, COMMAND | PARTIAL            },
      {6, COMMAND | SEMANTIC_UNRESOLVED},
      {2, PATH                         },
      {2, PATH | PARTIAL               },
      {2, PATH | INVALID               },
      {2, URL                          },
      {7, 0                            },
  };
  static_assert(countof(STYLES) == static_cast<usize>(highlight_role::count));
  if (role == highlight_role::count) return {1, 0};

  return STYLES[static_cast<usize>(role)];
}

pure fn Server::find_document(StringView uri) wontthrow -> Document *
{
  for (let &document : m_documents)
    if (document.uri == uri) return &document;

  return nullptr;
}

pure fn Server::request_document(const JsonValue *params) wontthrow
    -> Document *
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  return uri.has_value() ? find_document(*uri) : nullptr;
}

pure fn Server::request_positioned_document(const JsonValue *params) throws
    -> Maybe<positioned_document>
{
  let *document = request_document(params);
  if (document == nullptr) return None;
  let const position =
      document_position(params != nullptr ? params->get("position") : nullptr);
  if (!position.has_value()) return None;

  return positioned_document{document, *position};
}

fn Server::select_document_mood(const Document &document) wontthrow -> void
{
  m_context.set_mood(document.mood);
  m_context.apply_strictness_for_mood();
  m_context.set_warning_level(3);
  m_context.set_diagnostics_disabled(false);
  m_context.set_annoying_diagnostics_enabled(true);
}

fn Server::read_source(const Path &canonical_path) throws -> Maybe<String>
{
  for (let const &document : m_documents) {
    if (document.canonical_path.has_value() &&
        document.canonical_path->text() == canonical_path.text())
      return document.normalized_source.clone();
  }

  return None;
}

fn Server::initialize(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  if (params != nullptr && params->kind == json_kind::Object) {
    if (let const root_uri = string_field(params, "rootUri");
        root_uri.has_value())
    {
      if (let path = decode_file_uri(*root_uri); path.has_value())
        m_workspace_root = path.take();
    }
    let const *capabilities = params->get("capabilities");
    let const *code_action =
        json_field_path(capabilities, "textDocument", "codeAction");
    let const *kind_values = json_field_path(
        code_action, "codeActionLiteralSupport", "codeActionKind", "valueSet");

    m_supports_document_changes = json_field_is_true(json_field_path(
        capabilities, "workspace", "workspaceEdit", "documentChanges"));
    m_supports_code_action_literals =
        kind_values != nullptr && kind_values->kind == json_kind::Array;
    m_supports_quick_fixes = false;
    m_supports_fix_all = false;
    if (m_supports_code_action_literals) {
      for (let const *kind : kind_values->array) {
        if (kind->kind != json_kind::String) continue;
        if (code_action_kind_includes(kind->text, "quickfix"))
          m_supports_quick_fixes = true;
        if (code_action_kind_includes(kind->text, "source.fixAll.kosh"))
          m_supports_fix_all = true;
      }
    }

    m_supports_preferred_actions =
        json_field_is_true(json_field_path(code_action, "isPreferredSupport"));
    m_supports_diagnostic_data = json_field_is_true(json_field_path(
        capabilities, "textDocument", "publishDiagnostics", "dataSupport"));
    m_supports_markdown_hover = json_array_holds(
        json_field_path(capabilities, "textDocument", "hover", "contentFormat"),
        "markdown");

    if (json_array_holds(
            json_field_path(capabilities, "general", "positionEncodings"),
            "utf-8"))
    {
      m_encoding = position_encoding::Utf8;
    }
  }
  m_is_initialized = true;
  let const encoding =
      m_encoding == position_encoding::Utf8 ? "utf-8" : "utf-16";
  let result = String{"{\"capabilities\":{\"positionEncoding\":"};
  append_json_string(result, encoding);
  result.append(",\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                "\"completionProvider\":{\"resolveProvider\":true},"
                "\"definitionProvider\":true,\"hoverProvider\":true,"
                "\"documentSymbolProvider\":true,"
                "\"renameProvider\":{\"prepareProvider\":true},");
  if (m_supports_quick_fixes || m_supports_fix_all) {
    result.append("\"codeActionProvider\":{\"codeActionKinds\":[");
    if (m_supports_quick_fixes) result.append("\"quickfix\"");
    if (m_supports_quick_fixes && m_supports_fix_all) result.push(',');
    if (m_supports_fix_all) result.append("\"source.fixAll.kosh\"");
    result.append("],\"resolveProvider\":false},");
  }
  result.append(
      "\"semanticTokensProvider\":{\"legend\":{\"tokenTypes\":["
      "\"comment\",\"operator\",\"string\",\"variable\",\"parameter\","
      "\"keyword\",\"function\",\"regexp\",\"heredocDelimiter\"],"
      "\"tokenModifiers\":["
      "\"declaration\",\"readonly\",\"invalid\",\"unresolved\","
      "\"partial\",\"path\",\"command\",\"heredoc\",\"url\","
      "\"set\",\"unused\"]},"
      "\"full\":true,\"range\":false}},\"serverInfo\":{\"name\":"
      "\"kosh\"}}");

  return send_result(id, result.view());
}

fn Server::open_document(const JsonValue *params) throws -> Document *
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  let const language_id = string_field(text_document, "languageId");
  let const text = string_field(text_document, "text");
  let const version = integer_field(text_document, "version");
  if (!uri.has_value() || !language_id.has_value() || !text.has_value())
    return nullptr;
  let const document_version = version.value_or(0);
  m_highlight_cache = completion::shell_highlight_cache{};
  if (let *existing = find_document(*uri); existing != nullptr) {
    existing->language_id = String{*language_id};
    existing->replace_source(*text, document_version);
    existing->mood = mood_for(*existing);
    return existing;
  }
  let document = Document{*uri, *language_id, *text, document_version};
  document.path = decode_file_uri(*uri);
  if (document.path.has_value())
    document.canonical_path = os::canonical_path(*document.path);
  document.mood = mood_for(document);
  m_documents.push(steal(document));

  return &m_documents.back();
}

fn Server::change_document(const JsonValue *params) throws -> Document *
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  let const version = integer_field(text_document, "version");
  let const *changes =
      params != nullptr ? params->get("contentChanges") : nullptr;
  if (!uri.has_value() || changes == nullptr ||
      changes->kind != json_kind::Array || changes->array.is_empty())
    return nullptr;
  let *document = find_document(*uri);
  if (document == nullptr) return nullptr;
  let const text = string_field(changes->array.back(), "text");
  if (!text.has_value()) return nullptr;
  m_highlight_cache = completion::shell_highlight_cache{};
  document->replace_source(*text, version.value_or(document->version + 1));
  document->mood = mood_for(*document);

  return document;
}

fn Server::send_empty_diagnostics(StringView uri) throws -> bool
{
  let payload = String{"{\"jsonrpc\":\"2.0\",\"method\":"
                       "\"textDocument/publishDiagnostics\",\"params\":{"
                       "\"uri\":"};
  append_json_string(payload, uri);
  payload.append(",\"diagnostics\":[]}}");

  return send_payload(payload.view());
}

fn Server::close_document(const JsonValue *params) throws -> void
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  if (!uri.has_value()) return;
  send_empty_diagnostics(*uri);

  for (usize index = 0; index < m_documents.count(); index++) {
    if (m_documents[index].uri != *uri) continue;
    m_documents.remove(index);
    break;
  }
}

fn Server::append_diagnostic(String &output, const Document &document,
                             const source_diagnostic &diagnostic,
                             usize diagnostic_index, bool &is_first) throws
    -> void
{
  if (!diagnostic.source_name.is_empty() && document.path.has_value() &&
      diagnostic.source_name != document.path->text())
    return;
  let const do_append_diagnostic =
      [&](const SourceLocation &location, u8 severity, StringView message,
          Maybe<diagnostic_id> id, bool has_fixes) throws -> void {
    if (!is_first) output.push(',');
    is_first = false;
    output.append("{\"range\":");
    append_protocol_range(output, document, location.position,
                          location.position + location.length, m_encoding);
    output.append(",\"severity\":");
    append_json_integer(output, static_cast<u64>(severity));
    if (id.has_value()) {
      output.append(",\"code\":");
      append_json_string(output, get_diagnostic_definition(*id).slug);
    }
    output.append(",\"source\":\"kosh\",\"message\":");
    append_json_string(output, message);
    if (has_fixes && m_supports_diagnostic_data && document.version >= 0) {
      output.append(",\"data\":{\"kind\":\"kosh.fix\","
                    "\"documentVersion\":");
      append_json_integer(output, document.version);
      output.append(",\"diagnosticRevision\":");
      append_json_integer(output, document.diagnostic_revision);
      output.append(",\"diagnosticIndex\":");
      append_json_integer(output, static_cast<u64>(diagnostic_index));
      output.push('}');
    }
    output.push('}');
  };

  let const severity = diagnostic.severity == error_severity::Error     ? 1
                       : diagnostic.severity == error_severity::Warning ? 2
                                                                        : 3;
  if (!diagnostic.message.is_empty())
    do_append_diagnostic(diagnostic.location, severity,
                         diagnostic.message.view(), diagnostic.id,
                         !diagnostic.fixes.is_empty());
  if (diagnostic.id.has_value() &&
      (*diagnostic.id == diagnostic_id::unresolved_command ||
       *diagnostic.id == diagnostic_id::unresolved_command_uncertain))
  {
    do_append_diagnostic(
        diagnostic.location, 3,
        "This command may be defined dynamically or outside this script", None,
        false);
  }
  if (!diagnostic.suggestion.is_empty())
    do_append_diagnostic(diagnostic.location, 3, diagnostic.suggestion.view(),
                         None, false);
  if (diagnostic.related_location.has_value() &&
      !diagnostic.related_message.is_empty() &&
      (diagnostic.related_source_name.is_empty() ||
       !document.path.has_value() ||
       diagnostic.related_source_name == document.path->text()))
  {
    do_append_diagnostic(*diagnostic.related_location, 3,
                         diagnostic.related_message.view(), None, false);
  }
}

fn Server::publish_diagnostics(Document &document) throws -> bool
{
  select_document_mood(document);
  let const arena_mark = m_ast_arena.mark();
  defer { m_ast_arena.release(arena_mark); };
  let const filename = document.path.has_value() ? document.path->text().view()
                                                 : document.uri.view();
  let parser = Parser{
      Lexer{document.normalized_source.view(), m_ast_arena, false, filename,
            m_context.mood()}
  };
  parser.set_should_collect_analysis_scopes(true);
  let rendered_errors = ArrayList<String>{heap_allocator()};
  let diagnostics = ArrayList<source_diagnostic>{heap_allocator()};
  let followed_paths = HashSet{heap_allocator()};
  /* A document that does not parse leaves the records empty. Hover then answers
     nothing. */
  let symbol_records = analysis_symbol_records{};
  let const ast =
      parser.construct_ast(rendered_errors, &m_context, &diagnostics);
  if (rendered_errors.is_empty()) {
    let const suppressions = parser.take_shellcheck_suppressions();
    let const scopes = parser.take_analysis_scope_definitions();
    let const directives = parser.take_shellcheck_directive_spans();
    let const heredoc_misses = parser.take_heredoc_terminator_misses();
    let const functions = m_context.function_names();
    let const aliases = m_context.alias_names();
    let source_effects = StringMap<followed_source_effects>{heap_allocator()};
    if (document.canonical_path.has_value())
      followed_paths.add(document.canonical_path->text().view());
    analyze_ast(ast, document.normalized_source.view(), functions, aliases,
                &m_context, 3, false, m_context.mood() == mimic_mood::Default,
                true, suppressions, scopes, directives, heredoc_misses,
                document.path.has_value(), false, &followed_paths,
                &source_effects, nullptr, nullptr, true, true, nullptr,
                &diagnostics, this, &symbol_records);
    symbol_records.variable_occurrences.sort(
        [](const variable_occurrence_record &left,
           const variable_occurrence_record &right) {
          if (left.position != right.position)
            return left.position < right.position;
          return left.length < right.length;
        });
  }

  let root_diagnostics = ArrayList<source_diagnostic>{heap_allocator()};
  let auxiliary_diagnostics = ArrayList<source_diagnostic>{heap_allocator()};
  let const root_name = document.path.has_value() ? document.path->text().view()
                                                  : document.uri.view();
  for (let const &diagnostic : diagnostics) {
    if (diagnostic.source_name.is_empty() ||
        diagnostic.source_name == root_name)
    {
      root_diagnostics.push(diagnostic);
    } else {
      auxiliary_diagnostics.push(diagnostic);
    }
  }

  document.diagnostic_revision++;
  let const did_send = send_diagnostics(document, root_diagnostics);
  document.diagnostics = steal(root_diagnostics);
  document.auxiliary_diagnostics = steal(auxiliary_diagnostics);
  document.followed_paths = steal(followed_paths);
  document.symbol_records = steal(symbol_records);

  return did_send;
}

fn Server::send_diagnostics(
    const Document &document,
    const ArrayList<source_diagnostic> &diagnostics) throws -> bool
{
  let payload = String{"{\"jsonrpc\":\"2.0\",\"method\":"
                       "\"textDocument/publishDiagnostics\",\"params\":{"
                       "\"uri\":"};
  append_json_string(payload, document.uri.view());
  if (document.version >= 0) {
    payload.append(",\"version\":");
    append_json_integer(payload, document.version);
  }
  payload.append(",\"diagnostics\":[");
  bool is_first = true;
  for (usize diagnostic_index = 0; diagnostic_index < diagnostics.count();
       diagnostic_index++)
    append_diagnostic(payload, document, diagnostics[diagnostic_index],
                      diagnostic_index, is_first);
  payload.append("]}}");

  return send_payload(payload.view());
}

fn Server::publish_auxiliary_diagnostics() throws -> bool
{
  for (let const &diagnostic : m_current_auxiliary_diagnostics) {
    let const source_path = Path{diagnostic.source_name.view()};
    let source_uri = file_uri_for_path(source_path);
    if (m_current_auxiliary_uris.find(source_uri).has_value()) continue;
    bool is_open_source = find_document(source_uri.view()) != nullptr;
    if (!is_open_source) {
      let const canonical_source = os::canonical_path(source_path);
      for (let const &document : m_documents) {
        if (!canonical_source.has_value() ||
            !document.canonical_path.has_value())
          continue;
        if (document.canonical_path->text() == canonical_source->text()) {
          is_open_source = true;
          break;
        }
      }
    }
    if (is_open_source) continue;
    let source = read_source(source_path);
    if (!source.has_value()) source = source_path.read_entire_file();
    if (!source.has_value()) continue;
    let auxiliary =
        Document{source_uri.view(), "shellscript", source->view(), -1};
    auxiliary.path = source_path;
    let source_diagnostics = ArrayList<source_diagnostic>{heap_allocator()};
    for (let const &candidate : m_current_auxiliary_diagnostics)
      if (candidate.source_name == diagnostic.source_name)
        source_diagnostics.push(candidate);
    if (!send_diagnostics(auxiliary, source_diagnostics)) return false;
    m_current_auxiliary_uris.push(steal(source_uri));
  }

  return true;
}

fn Server::validate_all(Document *changed_document) throws -> bool
{
  m_current_auxiliary_uris.clear();
  m_current_auxiliary_diagnostics.clear();
  for (let &document : m_documents) {
    let should_reanalyze =
        changed_document == nullptr || &document == changed_document;
    if (!should_reanalyze && changed_document != nullptr &&
        changed_document->canonical_path.has_value())
    {
      should_reanalyze = document.followed_paths.contains(
          changed_document->canonical_path->text().view());
    }
    if (should_reanalyze && !publish_diagnostics(document)) return false;

    for (let const &diagnostic : document.auxiliary_diagnostics)
      m_current_auxiliary_diagnostics.push(diagnostic);
  }
  if (!publish_auxiliary_diagnostics()) return false;

  for (let const &uri : m_published_auxiliary_uris) {
    if (m_current_auxiliary_uris.find(uri).has_value()) continue;
    if (!send_empty_diagnostics(uri.view())) return false;
  }
  m_published_auxiliary_uris = steal(m_current_auxiliary_uris);

  return true;
}

fn Server::complete(const JsonValue *id, const JsonValue *params) throws -> bool
{
  let const request = request_positioned_document(params);
  if (!request.has_value()) return send_result(id, "[]");
  let *document = request->document;
  let const cursor = document->byte_position(
      request->position.line, request->position.character, m_encoding);
  if (!cursor.has_value()) return send_result(id, "[]");
  select_document_mood(*document);
  let base_directory = m_workspace_root;
  if (document->path.has_value()) base_directory = document->path->parent();
  let &resolver = m_context.get_program_resolver();
  resolver.begin_explicit_completion(
      ProgramResolver::CompletionRefresh::Cached);
  defer { resolver.end_explicit_completion(); };
  let result = completion::complete(document->normalized_source.view(), *cursor,
                                    m_context, base_directory,
                                    completion::completion_mode::Listing);
  let response = String{"["};

  for (usize index = 0; index < result.candidates.count(); index++) {
    if (index != 0) response.push(',');
    let const &candidate = result.candidates[index];
    response.append("{\"label\":");
    append_json_string(response, candidate.view());
    if (let const *description = result.descriptions.find(candidate.view());
        description != nullptr)
    {
      response.append(",\"detail\":");
      append_json_string(response, description->view());
    }
    if (result.is_command_position &&
        !os::has_directory_separator(candidate.view()))
    {
      /* 14 is CompletionItemKind.Keyword and 3 is
         CompletionItemKind.Function. */
      if (KEYWORDS.find(candidate.view()).has_value())
        response.append(",\"kind\":14,\"data\":{\"command\":");
      else
        response.append(",\"kind\":3,\"data\":{\"command\":");
      append_json_string(response, candidate.view());
      response.push('}');
    }
    response.append(",\"textEdit\":");
    append_text_edit(response, *document, result.token_start, result.token_end,
                     candidate.view());
    response.push('}');
  }
  response.push(']');

  return send_result(id, response.view());
}

fn Server::resolve_completion(const JsonValue *id,
                              const JsonValue *params) throws -> bool
{
  if (params == nullptr || params->kind != json_kind::Object)
    return send_result(id, "{}");
  let const *data = params->get("data");
  let const *command = data != nullptr ? data->get("command") : nullptr;

  let information = Maybe<String>{None};
  if (command != nullptr && command->kind == json_kind::String)
    information = command_information(command->text.view());

  let response = String{"{"};

  for (usize index = 0; index < params->object.count(); index++) {
    let const name = params->object[index].name.view();
    if (information.has_value() && name == "documentation") continue;
    if (response.count() > 1) response.push(',');
    append_json_string(response, name);
    response.push(':');
    append_json_value(response, *params->object[index].value);
  }
  if (information.has_value()) {
    if (response.count() > 1) response.push(',');
    response.append("\"documentation\":");
    append_json_string(response, information->view());
  }
  response.push('}');

  return send_result(id, response.view());
}

fn Server::append_text_edit(String &output, const Document &document,
                            usize start_position, usize end_position,
                            StringView replacement) throws -> void
{
  output.append("{\"range\":");
  append_protocol_range(output, document, start_position, end_position,
                        m_encoding);
  output.append(",\"newText\":");
  append_json_string(output, replacement);
  output.push('}');
}

fn Server::open_workspace_edit(String &output, const Document &document) throws
    -> void
{
  if (m_supports_document_changes) {
    output.append("\"documentChanges\":[{\"textDocument\":{\"uri\":");
    append_json_string(output, document.uri.view());
    output.append(",\"version\":");
    append_json_integer(output, document.version);
    output.append("},\"edits\":[");

    return;
  }

  output.append("\"changes\":{");
  append_json_string(output, document.uri.view());
  output.append(":[");
}

fn Server::close_workspace_edit(String &output) throws -> void
{
  output.append(m_supports_document_changes ? "]}]}" : "]}}");
}

fn Server::code_actions(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  if (!m_supports_code_action_literals) return send_result(id, "[]");
  let *document = request_document(params);
  if (document == nullptr) return send_result(id, "[]");

  usize range_start = 0;
  usize range_end = document->normalized_source.count();
  if (let const *range = params != nullptr ? params->get("range") : nullptr;
      range != nullptr)
  {
    let const start = document_position(range->get("start"));
    let const end = document_position(range->get("end"));
    if (!start.has_value() || !end.has_value()) return send_result(id, "[]");
    let const start_byte =
        document->byte_position(start->line, start->character, m_encoding);
    let const end_byte =
        document->byte_position(end->line, end->character, m_encoding);
    if (!start_byte.has_value() || !end_byte.has_value())
      return send_result(id, "[]");
    range_start = *start_byte;
    range_end = *end_byte;
    if (range_end < range_start) return send_result(id, "[]");
  }

  bool should_include_quick_fixes = m_supports_quick_fixes;
  bool should_include_fix_all = m_supports_fix_all;
  let const *context = params != nullptr ? params->get("context") : nullptr;
  let const *only = context != nullptr ? context->get("only") : nullptr;
  if (only != nullptr && only->kind == json_kind::Array) {
    should_include_quick_fixes = false;
    should_include_fix_all = false;
    for (let const *kind : only->array) {
      if (kind->kind != json_kind::String) continue;
      if (m_supports_quick_fixes &&
          code_action_kind_includes(kind->text, "quickfix"))
        should_include_quick_fixes = true;
      if (m_supports_fix_all &&
          code_action_kind_includes(kind->text, "source.fixAll.kosh"))
        should_include_fix_all = true;
    }
  }

  let response = String{"["};
  bool is_first_action = true;
  let const *context_diagnostics =
      context != nullptr ? context->get("diagnostics") : nullptr;
  let const do_associated_diagnostic =
      [&](usize diagnostic_index, const source_diagnostic &diagnostic)
          wontthrow -> const JsonValue * {
    if (!m_supports_diagnostic_data || context_diagnostics == nullptr ||
        context_diagnostics->kind != json_kind::Array ||
        !diagnostic.id.has_value())
    {
      return nullptr;
    }

    for (let const *candidate : context_diagnostics->array) {
      if (candidate->kind != json_kind::Object) continue;
      let const code = string_field(candidate, "code");
      if (!code.has_value() ||
          *code != get_diagnostic_definition(*diagnostic.id).slug)
        continue;
      let const *data = candidate->get("data");
      let const kind = string_field(data, "kind");
      let const version = integer_field(data, "documentVersion");
      let const revision = integer_field(data, "diagnosticRevision");
      let const index = integer_field(data, "diagnosticIndex");
      if (!kind.has_value() || *kind != "kosh.fix" || !version.has_value() ||
          *version != document->version || !revision.has_value() ||
          *revision < 0 ||
          static_cast<u64>(*revision) != document->diagnostic_revision ||
          !index.has_value() || *index < 0 ||
          static_cast<usize>(*index) != diagnostic_index)
        continue;
      let const *candidate_range = candidate->get("range");
      if (candidate_range == nullptr ||
          candidate_range->kind != json_kind::Object)
        continue;
      let const start = document_position(candidate_range->get("start"));
      let const end = document_position(candidate_range->get("end"));
      if (!start.has_value() || !end.has_value()) continue;
      let const start_byte =
          document->byte_position(start->line, start->character, m_encoding);
      let const end_byte =
          document->byte_position(end->line, end->character, m_encoding);
      if (!start_byte.has_value() || !end_byte.has_value() ||
          *start_byte != diagnostic.location.position ||
          *end_byte !=
              diagnostic.location.position + diagnostic.location.length)
        continue;

      return candidate;
    }

    return nullptr;
  };
  let const do_append_workspace_edit =
      [&](String &output, const ArrayList<const source_edit *> &edits)
          throws -> void {
    output.append("\"edit\":{");
    open_workspace_edit(output, *document);

    for (usize edit_index = 0; edit_index < edits.count(); edit_index++) {
      if (edit_index != 0) output.push(',');
      append_text_edit(output, *document, edits[edit_index]->start_position,
                       edits[edit_index]->end_position,
                       edits[edit_index]->replacement.view());
    }

    close_workspace_edit(output);
  };
  let const do_append_action = [&](StringView title, StringView kind,
                                   const ArrayList<const source_edit *> &edits,
                                   bool is_preferred,
                                   const JsonValue *diagnostic) throws -> void {
    if (!is_first_action) response.push(',');
    is_first_action = false;
    response.append("{\"title\":");
    append_json_string(response, title);
    response.append(",\"kind\":");
    append_json_string(response, kind);
    if (is_preferred && m_supports_preferred_actions)
      response.append(",\"isPreferred\":true");
    if (diagnostic != nullptr) {
      response.append(",\"diagnostics\":[");
      append_json_value(response, *diagnostic);
      response.push(']');
    }
    response.push(',');
    do_append_workspace_edit(response, edits);
    response.push('}');
  };

  if (should_include_quick_fixes) {
    let edits = ArrayList<const source_edit *>{heap_allocator()};
    for (usize diagnostic_index = 0;
         diagnostic_index < document->diagnostics.count(); diagnostic_index++)
    {
      let const &diagnostic = document->diagnostics[diagnostic_index];
      let const diagnostic_end =
          diagnostic.location.position + diagnostic.location.length;
      if (range_start == range_end) {
        if (diagnostic.location.length == 0) {
          if (range_start != diagnostic.location.position) continue;
        } else if (range_start < diagnostic.location.position ||
                   range_start >= diagnostic_end)
        {
          continue;
        }
      } else if (diagnostic_end <= range_start ||
                 diagnostic.location.position >= range_end)
      {
        continue;
      }
      for (let const &fix : diagnostic.fixes) {
        edits.clear();
        for (let const &edit : fix.edits)
          edits.push(&edit);
        do_append_action(
            fix.title.view(), "quickfix", edits, fix.is_preferred,
            do_associated_diagnostic(diagnostic_index, diagnostic));
      }
    }
  }

  if (should_include_fix_all) {
    let candidates = ArrayList<const source_edit *>{heap_allocator()};
    for (let const &diagnostic : document->diagnostics) {
      for (let const &fix : diagnostic.fixes) {
        if (!fix.is_safe_for_fix_all) continue;
        for (let const &edit : fix.edits)
          candidates.push(&edit);
      }
    }
    let const nonconflicting =
        select_nonconflicting_source_edits(steal(candidates));
    if (!nonconflicting.is_empty())
      do_append_action("Fix all safe kosh diagnostics", "source.fixAll.kosh",
                       nonconflicting, true, nullptr);
  }
  response.push(']');

  return send_result(id, response.view());
}

fn Server::symbol_at(const Document &document,
                     protocol_position position) throws
    -> Maybe<document_symbol>
{
  let const byte_position =
      document.byte_position(position.line, position.character, m_encoding);
  if (!byte_position.has_value() ||
      position.line >= document.line_starts.count())
    return None;
  let const[line_start, line_end] = document.get_line_bounds(position.line);
  let const *spans = m_highlight_cache.spans_for(
      document.normalized_source.view(), line_start, line_end, m_context);

  let touching = Maybe<document_symbol>{};

  for (let const &span : *spans) {
    let const start = line_start + span.start;
    let const end = line_start + span.end;
    if (*byte_position < start || *byte_position > end) continue;

    let symbol =
        document_symbol{String{document.normalized_source.substring_of_length(
                            start, end - start)},
                        span.role, start, end};

    /* A cursor sitting on a boundary belongs to the span that opens there. */
    if (*byte_position == end) {
      if (!touching.has_value()) touching = steal(symbol);
      continue;
    }

    return symbol;
  }

  return touching;
}

pure fn role_reads_variable(highlight_role role) wontthrow -> bool
{
  return role == highlight_role::variable ||
         role == highlight_role::unset_variable ||
         role == highlight_role::assignment_name;
}

pure fn role_reads_function(highlight_role role) wontthrow -> bool
{
  return role == highlight_role::function_name ||
         role == highlight_role::resolved_command ||
         role == highlight_role::unknown_command;
}

pure fn variable_name_start_of(StringView text) wontthrow -> usize
{
  usize start = 0;
  if (!text.is_empty() && text[0] == '$') start++;
  if (start < text.length && text[start] == '{') start++;

  return start;
}

pure fn variable_name_of(StringView text) wontthrow -> StringView
{
  let const start = variable_name_start_of(text);
  let end = start;

  while (end < text.length && lexer::is_variable_name(text[end]))
    end++;

  return text.substring_of_length(start, end - start);
}

pure fn hover_variable_name_of(StringView text) wontthrow -> StringView
{
  let const name = variable_name_of(text);
  if (!name.is_empty()) return name;

  if (text.length == 2 && text[0] == '$') return text.substring(1);

  if (text.length == 4 && text[0] == '$' && text[1] == '{' && text[3] == '}') {
    return text.substring_of_length(2, 1);
  }

  return StringView{};
}

fn Server::definition_of(const Document &document,
                         const document_symbol &symbol) throws
    -> Maybe<document_symbol>
{
  let const is_variable = role_reads_variable(symbol.role);
  let const is_function = role_reads_function(symbol.role);
  if (!is_variable && !is_function) return None;
  let const name =
      is_variable ? variable_name_of(symbol.text.view()) : symbol.text.view();
  if (name.is_empty()) return None;
  let result = Maybe<document_symbol>{};

  for (usize line = 0; line < document.line_starts.count(); line++) {
    let const[line_start, line_end] = document.get_line_bounds(line);
    let const *spans = m_highlight_cache.spans_for(
        document.normalized_source.view(), line_start, line_end, m_context);

    for (let const &span : *spans) {
      let const role_matches =
          is_variable ? span.role == highlight_role::assignment_name
                      : span.role == highlight_role::function_name;
      if (!role_matches) continue;
      let const start = line_start + span.start;
      let const end = line_start + span.end;
      let const candidate =
          document.normalized_source.substring_of_length(start, end - start);
      if (candidate != name) continue;
      if (start > symbol.start) return result;

      result = document_symbol{String{candidate}, span.role, start, end};
    }
  }

  return result;
}

fn Server::request_positioned_symbol(const JsonValue *params) throws
    -> Maybe<positioned_symbol>
{
  let const request = request_positioned_document(params);
  if (!request.has_value()) return None;
  select_document_mood(*request->document);
  let symbol = symbol_at(*request->document, request->position);
  if (!symbol.has_value()) return None;

  return positioned_symbol{request->document, steal(*symbol)};
}

fn Server::definition(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  let const found = request_positioned_symbol(params);
  if (!found.has_value()) return send_result(id, "null");
  let *document = found->document;
  let const *symbol = &found->symbol;
  let const target = definition_of(*document, *symbol);
  if (!target.has_value()) return send_result(id, "null");
  let response = String{"{\"uri\":"};
  append_json_string(response, document->uri.view());
  response.append(",\"range\":");
  append_protocol_range(response, *document, target->start, target->end,
                        m_encoding);
  response.push('}');

  return send_result(id, response.view());
}

/* The protocol code for a request the server understood and refused. */
constexpr i64 REQUEST_FAILED_ERROR = -32803;

/* A word that only prefixes a PATH entry still sits in command position, and it
   is renamed with the rest once the document defines the name. */
pure fn role_names_command(highlight_role role) wontthrow -> bool
{
  return role_reads_function(role) || role == highlight_role::partial_command;
}

pure fn spans_hold_definition(const ArrayList<rename_span> &spans) wontthrow
    -> bool
{
  for (let const &span : spans) {
    if (span.role == highlight_role::function_name) return true;
  }

  return false;
}

fn Server::rename_kind_of(const document_symbol &symbol) throws
    -> Maybe<rename_kind>
{
  if (role_reads_variable(symbol.role)) {
    if (!lexer::word_is_variable_name(variable_name_of(symbol.text.view())))
      return None;

    return rename_kind::variable;
  }

  if (!role_names_command(symbol.role)) return None;
  if (!completion::word_is_function_name(symbol.text.view())) return None;

  return rename_kind::command;
}

fn Server::collect_rename_spans(const Document &document, rename_kind kind,
                                StringView name,
                                ArrayList<rename_span> &out_spans) throws
    -> void
{
  for (usize line = 0; line < document.line_starts.count(); line++) {
    let const[line_start, line_end] = document.get_line_bounds(line);
    let const *spans = m_highlight_cache.spans_for(
        document.normalized_source.view(), line_start, line_end, m_context);

    for (let const &span : *spans) {
      let const is_wanted = kind == rename_kind::variable
                                ? role_reads_variable(span.role)
                                : role_names_command(span.role);
      if (!is_wanted) continue;
      let const start = line_start + span.start;
      let const end = line_start + span.end;
      let const text =
          document.normalized_source.substring_of_length(start, end - start);

      if (kind == rename_kind::command) {
        if (text != name) continue;
        out_spans.push(rename_span{span.role, start, end});
        continue;
      }

      if (variable_name_of(text) != name) continue;
      let const name_start = start + variable_name_start_of(text);
      out_spans.push(
          rename_span{span.role, name_start, name_start + name.length});
    }
  }
}

/* An alias and a function are defined in the open document, so a command name
   is renamed only when that definition is present. A program on PATH keeps its
   name everywhere. */
fn Server::prepare_rename(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  let const found = request_positioned_symbol(params);
  if (!found.has_value()) return send_result(id, "null");
  let *document = found->document;
  let const *symbol = &found->symbol;
  let const kind = rename_kind_of(*symbol);
  if (!kind.has_value()) return send_result(id, "null");
  let const is_variable = *kind == rename_kind::variable;
  let const name =
      is_variable ? variable_name_of(symbol->text.view()) : symbol->text.view();
  let spans = ArrayList<rename_span>{heap_allocator()};
  collect_rename_spans(*document, *kind, name, spans);

  if (!is_variable && !spans_hold_definition(spans)) {
    return send_result(id, "null");
  }

  let const name_start =
      symbol->start +
      (is_variable ? variable_name_start_of(symbol->text.view()) : 0);
  let response = String{"{\"range\":"};
  append_protocol_range(response, *document, name_start,
                        name_start + name.length, m_encoding);
  response.append(",\"placeholder\":");
  append_json_string(response, name);
  response.push('}');

  return send_result(id, response.view());
}

fn Server::rename(const JsonValue *id, const JsonValue *params) throws -> bool
{
  let const found = request_positioned_symbol(params);
  if (!found.has_value()) {
    return send_error(id, REQUEST_FAILED_ERROR,
                      "There is no renameable symbol here.");
  }
  let *document = found->document;
  let const *symbol = &found->symbol;
  let const kind = rename_kind_of(*symbol);
  if (!kind.has_value()) {
    return send_error(id, REQUEST_FAILED_ERROR,
                      "This symbol cannot be renamed.");
  }
  let const is_variable = *kind == rename_kind::variable;
  let const new_name = string_field(params, "newName");
  let const is_new_name_valid =
      new_name.has_value() &&
      (is_variable ? lexer::word_is_variable_name(*new_name)
                   : completion::word_is_function_name(*new_name));

  if (!is_new_name_valid) {
    return send_error(id, REQUEST_FAILED_ERROR,
                      is_variable
                          ? "A variable name holds letters, digits, and "
                            "underscores, and never opens with a digit."
                          : "A command name holds no shell metacharacter.");
  }

  let const name =
      is_variable ? variable_name_of(symbol->text.view()) : symbol->text.view();
  let spans = ArrayList<rename_span>{heap_allocator()};
  collect_rename_spans(*document, *kind, name, spans);

  if (!is_variable && !spans_hold_definition(spans)) {
    return send_error(id, REQUEST_FAILED_ERROR,
                      "This command is not defined in this document.");
  }

  let response = String{"{"};
  open_workspace_edit(response, *document);

  for (usize span_index = 0; span_index < spans.count(); span_index++) {
    if (span_index != 0) response.push(',');
    append_text_edit(response, *document, spans[span_index].start,
                     spans[span_index].end, *new_name);
  }

  close_workspace_edit(response);

  return send_result(id, response.view());
}

/* The protocol numbers a function 12 and a variable 13. */
constexpr usize OUTLINE_FUNCTION_KIND = 12;
constexpr usize OUTLINE_VARIABLE_KIND = 13;

struct outline_entry
{
  StringView name;
  usize kind;
  usize start;
  usize end;
  usize selection_start;
  usize selection_end;
};

struct outline_scope
{
  usize end;
  usize child_count;
  HashSet assigned_names;
};

fn document_outline(const Document &document) throws -> ArrayList<outline_entry>
{
  let entries = ArrayList<outline_entry>{heap_allocator()};
  let const source_length = document.normalized_source.count();

  for (let const &record : document.symbol_records.functions) {
    if (record.name_position >= source_length) continue;
    let const name_end = record.name_position + record.name.count();
    let const end = record.body_end_position > name_end
                        ? record.body_end_position
                        : name_end;
    entries.push(outline_entry{record.name.view(), OUTLINE_FUNCTION_KIND,
                               record.name_position, end, record.name_position,
                               name_end});
  }

  let const source = document.normalized_source.view();

  for (let const &record : document.symbol_records.assignments) {
    if (record.position >= source_length) continue;
    let const name_end = record.position + record.name.count();
    let const end = record.length > record.name.count()
                        ? record.position + record.length
                        : name_end;

    /* A quoted operand spells the name across quotes, so the name span is not
       a slice of the source and the whole entry is selected. */
    let const is_name_verbatim =
        name_end <= source_length &&
        source.substring_of_length(record.position, record.name.count()) ==
            record.name.view();

    entries.push(outline_entry{record.name.view(), OUTLINE_VARIABLE_KIND,
                               record.position, end, record.position,
                               is_name_verbatim ? name_end : end});
  }

  /* A container has to precede what it holds, so a wider span sorts first when
     two entries open together. */
  entries.sort([](const outline_entry &left, const outline_entry &right) {
    if (left.start != right.start) return left.start < right.start;

    return left.end > right.end;
  });

  return entries;
}

fn Server::document_symbols(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  let const *document = request_document(params);
  if (document == nullptr) return send_result(id, "[]");
  let const entries = document_outline(*document);
  let scopes = ArrayList<outline_scope>{heap_allocator()};
  let top_level_count = usize{0};
  let top_level_names = HashSet{heap_allocator()};
  let response = String{"["};
  let const do_close_scope = [&]() throws -> void {
    if (scopes.back().child_count > 0) response.push(']');
    response.push('}');
    scopes.pop_back();
  };

  for (let const &entry : entries) {
    while (!scopes.is_empty() && scopes.back().end <= entry.start)
      do_close_scope();

    /* One name assigned again in the same scope is one outline row. */
    if (entry.kind == OUTLINE_VARIABLE_KIND) {
      let &names =
          scopes.is_empty() ? top_level_names : scopes.back().assigned_names;
      if (!names.add(entry.name)) continue;
    }

    let const end = scopes.is_empty() || entry.end < scopes.back().end
                        ? entry.end
                        : scopes.back().end;
    if (scopes.is_empty()) {
      if (top_level_count > 0) response.push(',');
      top_level_count++;
    } else if (scopes.back().child_count > 0) {
      response.push(',');
      scopes.back().child_count++;
    } else {
      response.append(",\"children\":[");
      scopes.back().child_count++;
    }

    response.append("{\"name\":");
    append_json_string(response, entry.name);
    response.append(",\"kind\":");
    append_json_integer(response, static_cast<u64>(entry.kind));
    response.append(",\"range\":");
    append_protocol_range(response, *document, entry.start, end, m_encoding);
    response.append(",\"selectionRange\":");
    append_protocol_range(response, *document, entry.selection_start,
                          entry.selection_end, m_encoding);
    scopes.push(outline_scope{end, 0, HashSet{heap_allocator()}});
  }

  while (!scopes.is_empty())
    do_close_scope();

  response.push(']');

  return send_result(id, response.view());
}

fn Server::command_information(StringView command) throws -> Maybe<String>
{
  static constexpr u64 INFORMATION_TIMEOUT_NANOS = 5'000'000'000;
  let const do_run_shell_help = [&](String help_source)
                                    throws -> Maybe<String> {
    let argv = ArrayList<String>{heap_allocator()};
    argv.push(String{m_context.shell_executable_path()});
    argv.push(String{"--clean"});
    argv.push(String{"-c"});
    argv.push(steal(help_source));
    let output = os::capture_program_output(argv, INFORMATION_TIMEOUT_NANOS);
    if (output.has_value() && !output->is_empty()) return output;

    return None;
  };

  if (search_builtin(command).has_value()) {
    let source = String{"help "};
    source.append(command);

    return do_run_shell_help(steal(source));
  }

  let const paths = m_context.get_program_resolver().search(
      command, ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);

  /* A PATH program is what an ordinary command word resolves to, so the
     bundled utility answers only for a name PATH does not hold. */
  if (paths.is_empty()) {
    if (!m_context.koshkit_utilities_are_reachable()) return None;
    if (!koshkit::find_util(command).has_value()) return None;

    let source = String{"koshkit "};
    source.append(command);
    source.append(" --help");

    return do_run_shell_help(steal(source));
  }

  let information = String{completion::manpage_text_for(command, m_context)};
  if (information.is_empty())
    information = String{completion::help_text_of(command, m_context)};
  if (!information.is_empty()) {
    if (information.view()[information.length() - 1] != '\n')
      information.push('\n');
    information.push('\n');
  }
  information.append("Path: ");
  information.append(paths[0].text().view());

  return information;
}

/* A generated script would flood the card with assignment sites. */
static constexpr usize HOVER_EARLIER_ASSIGNMENT_LIMIT = 8;
static constexpr usize HOVER_ASSIGNMENT_TEXT_LENGTH_LIMIT = 200;
static constexpr usize HOVER_BODY_LINE_LIMIT = 40;

/* What each binder puts in the name, in the order of the enum. */
static constexpr StringView BINDER_DESCRIPTIONS[] = {
    "",
    "The name takes each word of the loop list in turn.",
    "The name takes the menu entry the reader selects.",
    "The value is the result of an arithmetic expression.",
    "The value is a field read from input.",
    "The value is a list of lines read from input.",
    "The value is the option letter the parse reached.",
    "The value is the formatted text.",
    "The name is declared and carries no value here.",
};
static_assert(countof(BINDER_DESCRIPTIONS) ==
              static_cast<usize>(assignment_binder::Declaration) + 1);

pure fn binder_description(assignment_binder binder) wontthrow -> StringView
{
  let const index = static_cast<usize>(binder);
  if (index >= countof(BINDER_DESCRIPTIONS)) return StringView{};

  return BINDER_DESCRIPTIONS[index];
}

pure fn source_line_span(const Document &document, usize position) wontthrow
    -> StringView
{
  let const source = document.normalized_source.view();
  let line_start = position;
  while (line_start > 0 && source[line_start - 1] != '\n')
    line_start--;

  while (line_start < position &&
         (source[line_start] == ' ' || source[line_start] == '\t'))
  {
    line_start++;
  }

  let const rest = source.substring(line_start);
  let const line_end = rest.find_character('\n');

  return line_end.has_value() ? rest.substring_of_length(0, *line_end) : rest;
}

/* An array assignment location covers the name and the operator alone, and a
   binding location covers the name alone. The rest is recovered from the
   line. */
pure fn assignment_headline_span(
    const Document &document,
    const variable_assignment_record &record) wontthrow -> StringView
{
  if (record.position >= document.normalized_source.count())
    return StringView{};

  if (record.binder != assignment_binder::Assignment)
    return source_line_span(document, record.position);

  let const span = document.normalized_source.substring_of_length(
      record.position, record.length);
  if (span.is_empty() || span[span.length - 1] != '=') return span;

  let const rest = document.normalized_source.substring(record.position);
  let const line_end = rest.find_character('\n');

  return line_end.has_value() ? rest.substring_of_length(0, *line_end) : rest;
}

fn append_hover_line_number(String &output, const Document &document,
                            usize position, position_encoding encoding) throws
    -> void
{
  let const line = document.protocol_position_at(position, encoding).line;
  output.append("line ");
  output.append(String::from(line + 1, heap_allocator()).view());
}

fn append_hover_clipped_text(String &output, StringView text,
                             usize length_limit) throws -> void
{
  if (text.length <= length_limit) {
    output.append(text);
    return;
  }

  output.append(text.substring_of_length(0, length_limit));
  output.append("...");
}

/* A marker line is appended outside the fence. The two content kinds stay
   structurally identical. */
fn append_hover_block(String &output, StringView text, bool is_markdown,
                      StringView language) throws -> void
{
  if (!is_markdown) {
    output.append(text);
    return;
  }

  /* A fence longer than the longest backquote run inside the text keeps a
     command substitution from closing the block early. */
  usize longest_backquote_run = 0;
  usize position = 0;
  while (position < text.length) {
    if (text[position] != '`') {
      position++;
      continue;
    }

    let const run_start = position;
    while (position < text.length && text[position] == '`')
      position++;
    if (position - run_start > longest_backquote_run)
      longest_backquote_run = position - run_start;
  }

  let const fence_length =
      longest_backquote_run < 3 ? 3 : longest_backquote_run + 1;
  let const do_append_fence = [&]() throws -> void {
    for (usize count = 0; count < fence_length; count++)
      output.push('`');
  };

  do_append_fence();
  output.append(language);
  output.push('\n');
  output.append(text);
  output.push('\n');
  do_append_fence();
}

pure fn clipped_line_span(StringView text, usize line_limit,
                          usize &dropped_line_count) wontthrow -> StringView
{
  dropped_line_count = 0;

  usize kept_line_count = 0;
  usize position = 0;
  while (position < text.length) {
    if (text[position] == '\n') {
      kept_line_count++;
      if (kept_line_count == line_limit) break;
    }
    position++;
  }

  if (position + 1 >= text.length) return text;

  for (usize scan = position; scan < text.length; scan++) {
    if (text[scan] == '\n') dropped_line_count++;
  }

  return text.substring_of_length(0, position);
}

fn assignments_reaching(const Document &document,
                        const document_symbol &symbol) throws
    -> ArrayList<const variable_assignment_record *>
{
  let reaching =
      ArrayList<const variable_assignment_record *>{heap_allocator()};
  let const name = variable_name_of(symbol.text.view());
  if (name.is_empty()) return reaching;

  for (let const &record : document.symbol_records.assignments) {
    if (record.position > symbol.start) continue;
    if (record.name.view() != name) continue;

    reaching.push(&record);
  }

  /* The walk is in source order today, and the sort keeps that out of the
     contract. */
  reaching.sort([](const variable_assignment_record *left,
                   const variable_assignment_record *right) {
    return left->position > right->position;
  });

  return reaching;
}

pure fn function_body_for(const Document &document,
                          const document_symbol &symbol) wontthrow
    -> const function_body_record *
{
  const function_body_record *nearest = nullptr;

  for (let const &record : document.symbol_records.functions) {
    if (record.name_position > symbol.start) continue;
    if (record.name.view() != symbol.text.view()) continue;
    if (nearest != nullptr && record.name_position < nearest->name_position)
      continue;

    nearest = &record;
  }

  return nearest;
}

fn Server::variable_hover_text(
    const Document &document,
    const ArrayList<const variable_assignment_record *> &reaching) throws
    -> String
{
  let const &nearest = *reaching.front();
  let headline = String{heap_allocator()};
  append_hover_clipped_text(headline,
                            assignment_headline_span(document, nearest),
                            HOVER_ASSIGNMENT_TEXT_LENGTH_LIMIT);

  let text = String{heap_allocator()};
  append_hover_block(text, headline.view(), m_supports_markdown_hover,
                     StringView{"shell"});

  if (nearest.binder != assignment_binder::Assignment) {
    text.push('\n');
    text.append(binder_description(nearest.binder));
  } else if (nearest.is_array) {
    text.append("\nThe value is a list, and the elements are not folded.");
  } else if (nearest.is_append) {
    text.append("\nThe value appends to what came before.");
  } else if (nearest.literal_value.has_value()) {
    text.append("\nValue: ");
    append_hover_clipped_text(text, nearest.literal_value->view(),
                              HOVER_ASSIGNMENT_TEXT_LENGTH_LIMIT);
  } else {
    text.append("\nThe value is known only at run time.");
  }

  if (nearest.is_conditional)
    text.append("\nThe assignment does not run on every path.");

  let const earlier_count = reaching.count() - 1;
  if (earlier_count == 0) return text;

  text.append("\n\nEarlier assignments:");
  if (m_supports_markdown_hover) text.push('\n');

  let const listed_count = earlier_count < HOVER_EARLIER_ASSIGNMENT_LIMIT
                               ? earlier_count
                               : HOVER_EARLIER_ASSIGNMENT_LIMIT;
  for (usize index = 1; index <= listed_count; index++) {
    let const &record = *reaching[index];
    text.push('\n');

    if (m_supports_markdown_hover) text.append("- ");
    append_hover_line_number(text, document, record.position, m_encoding);
    text.append(": ");

    if (m_supports_markdown_hover) text.push('`');
    append_hover_clipped_text(text, assignment_headline_span(document, record),
                              HOVER_ASSIGNMENT_TEXT_LENGTH_LIMIT);
    if (m_supports_markdown_hover) text.push('`');

    if (record.is_conditional) text.append(" (conditional)");
  }

  /* A cap that stayed silent would read as complete coverage. */
  if (listed_count < earlier_count) {
    text.append("\n\n");
    text.append(
        String::from(earlier_count - listed_count, heap_allocator()).view());
    text.append(" earlier assignments are not shown.");
  }

  return text;
}

fn Server::append_shell_variable_facts(
    String &output, const shell_variable_description &description) throws
    -> void
{
  output.append(description.summary);

  if (has_shell_variable_fact(description.facts, shell_variable_fact::Dynamic))
    output.append("\nThe shell computes the value on each read.");

  if (has_shell_variable_fact(description.facts, shell_variable_fact::Array))
    output.append("\nThe value is a list.");

  if (has_shell_variable_fact(description.facts, shell_variable_fact::ReadOnly))
    output.append("\nThe name is read-only.");

  if (has_shell_variable_fact(description.facts, shell_variable_fact::Exported))
    output.append("\nThe name is exported to a child process.");

  if (has_shell_variable_fact(description.facts,
                              shell_variable_fact::Unmaintained))
    output.append("\nThis shell does not maintain the name.");

  if (has_shell_variable_fact(description.facts, shell_variable_fact::BashOnly))
  {
    output.append(m_context.mood() == mimic_mood::Posix
                      ? "\nThe sh mood is active, so the name is unavailable."
                      : "\nThe name is unavailable in the sh mood.");
    return;
  }

  if (has_shell_variable_fact(description.facts, shell_variable_fact::NotPosix))
    output.append("\nThe name is defined by bash and not by POSIX.");
}

fn Server::shell_variable_hover_text(
    StringView name, const shell_variable_description &description) throws
    -> String
{
  let const is_special_parameter = name.length == 1;

  let headline = String{heap_allocator()};
  if (is_special_parameter) headline.push('$');
  headline.append(name);

  let text = String{heap_allocator()};
  append_hover_block(text, headline.view(), m_supports_markdown_hover,
                     StringView{"shell"});
  text.push('\n');
  append_shell_variable_facts(text, description);

  return text;
}

fn Server::function_hover_text(const Document &document,
                               const function_body_record &record) throws
    -> String
{
  let body = StringView{};
  if (record.body_end_position > record.body_position) {
    body = document.normalized_source.substring_of_length(
        record.body_position, record.body_end_position - record.body_position);
  }

  usize dropped_line_count = 0;
  let const kept_body =
      clipped_line_span(body, HOVER_BODY_LINE_LIMIT, dropped_line_count);

  /* The rendered shape follows FunctionDefinition::evaluate_impl. The form is
     the one declare -f prints. */
  let definition = String{heap_allocator()};
  definition.append(record.name.view());
  definition.append(" () \n");
  definition.append(kept_body);

  let text = String{heap_allocator()};
  append_hover_block(text, definition.view(), m_supports_markdown_hover,
                     StringView{"shell"});

  if (dropped_line_count > 0) {
    text.append("\n\n");
    text.append(String::from(dropped_line_count, heap_allocator()).view());
    text.append(" more lines are not shown.");
  }

  return text;
}

fn Server::send_hover(const JsonValue *id, const Document &document,
                      const document_symbol &symbol, StringView value) throws
    -> bool
{
  let response = String{"{\"contents\":{\"kind\":"};
  response.append(m_supports_markdown_hover ? "\"markdown\"" : "\"plaintext\"");
  response.append(",\"value\":");
  append_json_string(response, value);
  response.append("},\"range\":");
  append_protocol_range(response, document, symbol.start, symbol.end,
                        m_encoding);
  response.push('}');

  return send_result(id, response.view());
}

fn Server::hover(const JsonValue *id, const JsonValue *params) throws -> bool
{
  let const found = request_positioned_symbol(params);
  if (!found.has_value()) return send_result(id, "null");
  let *document = found->document;
  let const *symbol = &found->symbol;

  if (role_reads_variable(symbol->role)) {
    let const name = hover_variable_name_of(symbol->text.view());
    let const described = describe_shell_variable(name);
    let const reaching = assignments_reaching(*document, *symbol);

    if (reaching.is_empty()) {
      if (!described.has_value()) return send_result(id, "null");

      return send_hover(id, *document, *symbol,
                        shell_variable_hover_text(name, *described).view());
    }

    let text = variable_hover_text(*document, reaching);
    if (described.has_value()) {
      text.append("\n\nThe shell also defines the name.\n");
      append_shell_variable_facts(text, *described);
    }

    return send_hover(id, *document, *symbol, text.view());
  }

  if (role_reads_function(symbol->role)) {
    if (let const *record = function_body_for(*document, *symbol);
        record != nullptr)
    {
      return send_hover(id, *document, *symbol,
                        function_hover_text(*document, *record).view());
    }
  }

  let const is_command = symbol->role == highlight_role::resolved_command ||
                         symbol->role == highlight_role::partial_command ||
                         symbol->role == highlight_role::unknown_command;
  if (!is_command) return send_result(id, "null");
  if (definition_of(*document, *symbol).has_value())
    return send_result(id, "null");
  let const information = command_information(symbol->text.view());
  if (!information.has_value()) return send_result(id, "null");
  let text = String{heap_allocator()};
  append_hover_block(text, information->view(), m_supports_markdown_hover,
                     StringView{});

  return send_hover(id, *document, *symbol, text.view());
}

fn Server::semantic_tokens(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  let *document = request_document(params);
  if (document == nullptr) return send_result(id, "{\"data\":[]}");
  select_document_mood(*document);
  let response = String{"{\"data\":["};
  usize previous_line = 0;
  usize previous_character = 0;
  bool is_first = true;
  usize occurrence_index = 0;

  for (usize line = 0; line < document->line_starts.count(); line++) {
    let const[line_start, line_end] = document->get_line_bounds(line);
    let const *spans = m_highlight_cache.spans_for(
        document->normalized_source.view(), line_start, line_end, m_context);
    for (let const &span : *spans) {
      let const absolute_start = line_start + span.start;
      let const absolute_end = line_start + span.end;
      let const start_character =
          document->encoded_length(line_start, absolute_start, m_encoding);
      let const delta_line = line - previous_line;
      let const delta_character = delta_line == 0
                                      ? start_character - previous_character
                                      : start_character;
      let const length =
          document->encoded_length(absolute_start, absolute_end, m_encoding);
      if (length == 0) continue;
      let const[type, base_modifiers] = semantic_style(span.role);
      let modifiers = base_modifiers;
      while (occurrence_index <
                 document->symbol_records.variable_occurrences.count() &&
             document->symbol_records.variable_occurrences[occurrence_index]
                         .position +
                     document->symbol_records
                         .variable_occurrences[occurrence_index]
                         .length <=
                 absolute_start)
      {
        occurrence_index++;
      }

      if (span.role == highlight_role::variable ||
          span.role == highlight_role::assignment_name ||
          span.role == highlight_role::unset_variable)
      {
        for (usize index = occurrence_index;
             index < document->symbol_records.variable_occurrences.count();
             index++)
        {
          let const &occurrence =
              document->symbol_records.variable_occurrences[index];
          if (occurrence.position > absolute_start) break;
          if (occurrence.position != absolute_start ||
              occurrence.length != absolute_end - absolute_start)
            continue;

          if (occurrence.kind == variable_occurrence_kind::Assignment) {
            modifiers |= SEMANTIC_DECLARATION;
            if (occurrence.is_unused) modifiers |= SEMANTIC_UNUSED;
          } else {
            if (occurrence.is_unresolved) {
              modifiers |= SEMANTIC_UNRESOLVED;
              modifiers &= ~SEMANTIC_SET;
            } else {
              modifiers |= SEMANTIC_SET;
              modifiers &= ~SEMANTIC_UNRESOLVED;
            }
          }
          break;
        }
      }

      if (!is_first) response.push(',');
      is_first = false;
      append_json_integer(response, static_cast<u64>(delta_line));
      response.push(',');
      append_json_integer(response, static_cast<u64>(delta_character));
      response.push(',');
      append_json_integer(response, static_cast<u64>(length));
      response.push(',');
      append_json_integer(response, static_cast<u64>(type));
      response.push(',');
      append_json_integer(response, static_cast<u64>(modifiers));
      previous_line = line;
      previous_character = start_character;
    }
  }
  response.append("]}");

  return send_result(id, response.view());
}

fn Server::dispatch(const JsonValue &message) throws -> bool
{
  if (message.kind != json_kind::Object) return true;
  let const *id = message.get("id");
  let const method = string_field(&message, "method");
  let const *params = message.get("params");
  if (!method.has_value()) {
    if (id != nullptr) return send_error(id, -32600, "Invalid Request");
    return true;
  }
  let const request = REQUEST_METHODS.find(*method);
  if (!request.has_value()) {
    if (id != nullptr) return send_error(id, -32601, "Method not found");
    return true;
  }

  if (*request == request_method::Initialize) return initialize(id, params);
  if (*request == request_method::Initialized) return true;
  if (*request == request_method::Shutdown) {
    m_is_shutdown = true;
    return send_result(id, "null");
  }
  if (*request == request_method::Exit) return false;
  if (!m_is_initialized) {
    if (id != nullptr) return send_error(id, -32002, "Server not initialized");
    return true;
  }
  if (m_is_shutdown) {
    if (id != nullptr) return send_error(id, -32600, "Server has shut down");
    return true;
  }
  switch (*request) {
  case request_method::DidOpen: return validate_all(open_document(params));
  case request_method::DidChange: return validate_all(change_document(params));
  case request_method::DidClose: close_document(params); return validate_all();
  case request_method::Completion: return complete(id, params);
  case request_method::ResolveCompletion: return resolve_completion(id, params);
  case request_method::CodeAction: return code_actions(id, params);
  case request_method::Definition: return definition(id, params);
  case request_method::Hover: return hover(id, params);
  case request_method::SemanticTokens: return semantic_tokens(id, params);
  case request_method::DocumentSymbols: return document_symbols(id, params);
  case request_method::PrepareRename: return prepare_rename(id, params);
  case request_method::Rename: return rename(id, params);
  case request_method::Initialize:
  case request_method::Initialized:
  case request_method::Shutdown:
  case request_method::Exit: return true;
  }

  return true;
}

fn Server::run() throws -> int
{
  let reader = ProtocolReader{};

  loop
  {
    let message = reader.read_message();
    if (!message.has_value()) return m_is_shutdown ? 0 : 1;
    let parser = JsonParser{*message};
    let *root = parser.parse();
    if (root == nullptr) {
      if (!send_error(nullptr, -32700, "Parse error")) return 1;
      continue;
    }
    if (!dispatch(*root)) return m_is_shutdown ? 0 : 1;
  }
}

} /* namespace */

fn run(EvalContext &context, BumpArena &ast_arena) throws -> int
{
  let server = Server{context, ast_arena};

  return server.run();
}

} /* namespace koshka::language_server */
