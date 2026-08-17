#include "LanguageServerProtocol.hpp"

namespace koshka::language_server {

namespace {

struct positioned_document
{
  Document *document;
  protocol_position position;
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
  CodeAction,
  Definition,
  Hover,
  SemanticTokens,
};

constexpr static_string_entry<request_method> REQUEST_METHOD_ENTRIES[] = {
    {SSK("initialize"),                       request_method::Initialize    },
    {SSK("initialized"),                      request_method::Initialized   },
    {SSK("shutdown"),                         request_method::Shutdown      },
    {SSK("exit"),                             request_method::Exit          },
    {SSK("textDocument/didOpen"),             request_method::DidOpen       },
    {SSK("textDocument/didChange"),           request_method::DidChange     },
    {SSK("textDocument/didClose"),            request_method::DidClose      },
    {SSK("textDocument/completion"),          request_method::Completion    },
    {SSK("textDocument/codeAction"),          request_method::CodeAction    },
    {SSK("textDocument/definition"),          request_method::Definition    },
    {SSK("textDocument/hover"),               request_method::Hover         },
    {SSK("textDocument/semanticTokens/full"), request_method::SemanticTokens},
};
constexpr StaticStringMap REQUEST_METHODS{REQUEST_METHOD_ENTRIES};

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
  fn definition(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn hover(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn semantic_tokens(const JsonValue *id, const JsonValue *params) throws
      -> bool;
  fn code_actions(const JsonValue *id, const JsonValue *params) throws -> bool;
  fn publish_diagnostics(Document &document) throws -> bool;
  fn send_diagnostics(const Document &document,
                      const ArrayList<source_diagnostic> &diagnostics) throws
      -> bool;
  fn publish_auxiliary_diagnostics() throws -> bool;
  fn validate_all(Document *changed_document = nullptr) throws -> bool;
  pure fn find_document(StringView uri) wontthrow -> Document *;
  pure fn request_document(const JsonValue *params) wontthrow -> Document *;
  pure fn request_positioned_document(const JsonValue *params) throws
      -> Maybe<positioned_document>;
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
};

pure fn semantic_style(highlight_role role) wontthrow -> std::pair<u32, u32>
{
  static constexpr u32 DECLARATION = 1u << 0;
  static constexpr u32 READONLY = 1u << 1;
  static constexpr u32 INVALID = 1u << 2;
  static constexpr u32 UNRESOLVED = 1u << 3;
  static constexpr u32 PARTIAL = 1u << 4;
  static constexpr u32 PATH = 1u << 5;
  static constexpr u32 COMMAND = 1u << 6;
  static constexpr u32 HEREDOC = 1u << 7;
  static constexpr u32 URL = 1u << 8;

  static constexpr std::pair<u32, u32> STYLES[] = {
      {0, 0                   },
      {1, 0                   },
      {2, 0                   },
      {2, HEREDOC             },
      {8, HEREDOC             },
      {3, 0                   },
      {3, DECLARATION         },
      {3, UNRESOLVED          },
      {4, READONLY            },
      {5, 0                   },
      {1, INVALID             },
      {6, DECLARATION         },
      {6, COMMAND             },
      {6, COMMAND | PARTIAL   },
      {6, COMMAND | UNRESOLVED},
      {2, PATH                },
      {2, PATH | PARTIAL      },
      {2, PATH | INVALID      },
      {2, URL                 },
      {7, 0                   },
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
    let const *workspace =
        capabilities != nullptr ? capabilities->get("workspace") : nullptr;
    let const *workspace_edit =
        workspace != nullptr ? workspace->get("workspaceEdit") : nullptr;
    let const *document_changes = workspace_edit != nullptr
                                      ? workspace_edit->get("documentChanges")
                                      : nullptr;
    m_supports_document_changes =
        document_changes != nullptr &&
        document_changes->kind == json_kind::Boolean &&
        document_changes->boolean;
    let const *text_document =
        capabilities != nullptr ? capabilities->get("textDocument") : nullptr;
    let const *code_action =
        text_document != nullptr ? text_document->get("codeAction") : nullptr;
    let const *literal_support =
        code_action != nullptr ? code_action->get("codeActionLiteralSupport")
                               : nullptr;
    let const *kind_support =
        literal_support != nullptr && literal_support->kind == json_kind::Object
            ? literal_support->get("codeActionKind")
            : nullptr;
    let const *kind_values =
        kind_support != nullptr && kind_support->kind == json_kind::Object
            ? kind_support->get("valueSet")
            : nullptr;
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
    let const *preferred_support = code_action != nullptr
                                       ? code_action->get("isPreferredSupport")
                                       : nullptr;
    m_supports_preferred_actions =
        preferred_support != nullptr &&
        preferred_support->kind == json_kind::Boolean &&
        preferred_support->boolean;
    let const *publish_diagnostics =
        text_document != nullptr ? text_document->get("publishDiagnostics")
                                 : nullptr;
    let const *data_support = publish_diagnostics != nullptr
                                  ? publish_diagnostics->get("dataSupport")
                                  : nullptr;
    m_supports_diagnostic_data = data_support != nullptr &&
                                 data_support->kind == json_kind::Boolean &&
                                 data_support->boolean;
    let const *general =
        capabilities != nullptr ? capabilities->get("general") : nullptr;
    let const *encodings =
        general != nullptr ? general->get("positionEncodings") : nullptr;
    if (encodings != nullptr && encodings->kind == json_kind::Array) {
      for (let const *encoding : encodings->array) {
        if (encoding->kind == json_kind::String && encoding->text == "utf-8") {
          m_encoding = position_encoding::Utf8;
          break;
        }
      }
    }
  }
  m_is_initialized = true;
  let const encoding =
      m_encoding == position_encoding::Utf8 ? "utf-8" : "utf-16";
  let result = String{"{\"capabilities\":{\"positionEncoding\":"};
  append_json_string(result, encoding);
  result.append(",\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
                "\"completionProvider\":{\"resolveProvider\":false},"
                "\"definitionProvider\":true,\"hoverProvider\":true,");
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
      "\"partial\",\"path\",\"command\",\"heredoc\",\"url\"]},"
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
  document->replace_source(*text, version.value_or(document->version + 1));
  document->mood = mood_for(*document);

  return document;
}

fn Server::close_document(const JsonValue *params) throws -> void
{
  let const *text_document =
      params != nullptr ? params->get("textDocument") : nullptr;
  let const uri = string_field(text_document, "uri");
  if (!uri.has_value()) return;
  let payload = String{"{\"jsonrpc\":\"2.0\",\"method\":"
                       "\"textDocument/publishDiagnostics\",\"params\":{"
                       "\"uri\":"};
  append_json_string(payload, *uri);
  payload.append(",\"diagnostics\":[]}}");
  send_payload(payload.view());

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
                &diagnostics, this);
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
    let payload = String{"{\"jsonrpc\":\"2.0\",\"method\":"
                         "\"textDocument/publishDiagnostics\",\"params\":{"
                         "\"uri\":"};
    append_json_string(payload, uri.view());
    payload.append(",\"diagnostics\":[]}}");
    if (!send_payload(payload.view())) return false;
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
    response.append(",\"textEdit\":{\"range\":");
    append_protocol_range(response, *document, result.token_start,
                          result.token_end, m_encoding);
    response.append(",\"newText\":");
    append_json_string(response, candidate.view());
    response.append("}}");
  }
  response.push(']');

  return send_result(id, response.view());
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
  let const do_append_edit = [&](String &output, const source_edit &edit)
                                 throws -> void {
    output.append("{\"range\":");
    append_protocol_range(output, *document, edit.start_position,
                          edit.end_position, m_encoding);
    output.append(",\"newText\":");
    append_json_string(output, edit.replacement.view());
    output.push('}');
  };
  let const do_append_workspace_edit =
      [&](String &output, const ArrayList<const source_edit *> &edits)
          throws -> void {
    output.append("\"edit\":{");
    if (m_supports_document_changes) {
      output.append("\"documentChanges\":[{\"textDocument\":{\"uri\":");
      append_json_string(output, document->uri.view());
      output.append(",\"version\":");
      append_json_integer(output, document->version);
      output.append("},\"edits\":[");
    } else {
      output.append("\"changes\":{");
      append_json_string(output, document->uri.view());
      output.append(":[");
    }
    for (usize edit_index = 0; edit_index < edits.count(); edit_index++) {
      if (edit_index != 0) output.push(',');
      do_append_edit(output, *edits[edit_index]);
    }
    output.append(m_supports_document_changes ? "]}]}" : "]}}");
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
  let const line_start = document.line_starts[position.line];
  let line_end = document.normalized_source.count();
  if (position.line + 1 < document.line_starts.count())
    line_end = document.line_starts[position.line + 1] - 1;
  let const *spans = m_highlight_cache.spans_for(
      document.normalized_source.view(), line_start, line_end, m_context);

  for (let const &span : *spans) {
    let const start = line_start + span.start;
    let const end = line_start + span.end;
    if (*byte_position < start || *byte_position > end) continue;

    return document_symbol{
        String{
            document.normalized_source.substring_of_length(start, end - start)},
        span.role, start, end};
  }

  return None;
}

pure fn variable_name_of(StringView text) wontthrow -> StringView
{
  usize start = 0;
  if (!text.is_empty() && text[0] == '$') start++;
  if (start < text.length && text[start] == '{') start++;
  let end = start;

  while (end < text.length) {
    let const byte = text[end];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '_'))
      break;
    end++;
  }

  return text.substring_of_length(start, end - start);
}

fn Server::definition_of(const Document &document,
                         const document_symbol &symbol) throws
    -> Maybe<document_symbol>
{
  let const is_variable = symbol.role == highlight_role::variable ||
                          symbol.role == highlight_role::unset_variable ||
                          symbol.role == highlight_role::assignment_name;
  let const is_function = symbol.role == highlight_role::function_name ||
                          symbol.role == highlight_role::resolved_command ||
                          symbol.role == highlight_role::unknown_command;
  if (!is_variable && !is_function) return None;
  let const name =
      is_variable ? variable_name_of(symbol.text.view()) : symbol.text.view();
  if (name.is_empty()) return None;
  let result = Maybe<document_symbol>{};

  for (usize line = 0; line < document.line_starts.count(); line++) {
    let const line_start = document.line_starts[line];
    let line_end = document.normalized_source.count();
    if (line + 1 < document.line_starts.count())
      line_end = document.line_starts[line + 1] - 1;
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

fn Server::definition(const JsonValue *id, const JsonValue *params) throws
    -> bool
{
  let const request = request_positioned_document(params);
  if (!request.has_value()) return send_result(id, "null");
  let *document = request->document;
  select_document_mood(*document);
  let const symbol = symbol_at(*document, request->position);
  if (!symbol.has_value()) return send_result(id, "null");
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

fn Server::command_information(StringView command) throws -> Maybe<String>
{
  static constexpr u64 INFORMATION_TIMEOUT_NANOS = 5'000'000'000;
  let source = String{heap_allocator()};
  if (search_builtin(command).has_value()) {
    source.append("help ");
    source.append(command);
  } else if (koshkit::find_util(command).has_value()) {
    source.append("koshkit ");
    source.append(command);
    source.append(" --help");
  }
  if (!source.is_empty()) {
    let argv = ArrayList<String>{heap_allocator()};
    argv.push(String{m_context.shell_executable_path()});
    argv.push(String{"--clean"});
    argv.push(String{"-c"});
    argv.push(steal(source));
    let output = os::capture_program_output(argv, INFORMATION_TIMEOUT_NANOS);
    if (output.has_value() && !output->is_empty()) return output;
    return None;
  }

  let const paths = m_context.get_program_resolver().search(
      command, ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (paths.is_empty()) return None;
  let information = String{heap_allocator()};
  let const man_paths = m_context.get_program_resolver().search(
      "man", ProgramResolver::SearchMode::First,
      ProgramResolver::Requirement::Runnable,
      ProgramResolver::CachePolicy::Bypass);
  if (!man_paths.is_empty() &&
      os::directory_is_trusted_for_exec(man_paths[0].parent()))
  {
    let locate_argv = ArrayList<String>{heap_allocator()};
    locate_argv.push(String{man_paths[0].text().view()});
    locate_argv.push(String{"-w"});
    locate_argv.push(String{command});
    let const location =
        os::capture_program_output(locate_argv, INFORMATION_TIMEOUT_NANOS);
    if (location.has_value() &&
        location->view().find_character('/').has_value())
    {
      let man_argv = ArrayList<String>{heap_allocator()};
      man_argv.push(String{man_paths[0].text().view()});
      man_argv.push(String{command});
      if (let page =
              os::capture_program_output(man_argv, INFORMATION_TIMEOUT_NANOS);
          page.has_value() && !page->is_empty())
      {
        let const page_length = page->length();
        for (usize position = 0; position < page_length; position++) {
          let const byte = page->view()[position];
          if (byte == '\b') {
            if (!information.is_empty()) information.pop_back();
            continue;
          }
          information.push(byte);
        }
      }
    }
  }
  if (information.is_empty()) {
    if (let const help_argument = completion::HELP_ALLOWLIST.find(command);
        help_argument.has_value() &&
        os::directory_is_trusted_for_exec(paths[0].parent()))
    {
      let help_argv = ArrayList<String>{heap_allocator()};
      help_argv.push(String{paths[0].text().view()});
      StringView{*help_argument}.for_each_ascii_whitespace_word(
          [&](StringView word) throws { help_argv.push(String{word}); });
      if (let help =
              os::capture_program_output(help_argv, INFORMATION_TIMEOUT_NANOS);
          help.has_value() && !help->is_empty())
        information = help.take();
    }
  }
  if (!information.is_empty()) {
    if (information.view()[information.length() - 1] != '\n')
      information.push('\n');
    information.push('\n');
  }
  information.append("Path: ");
  information.append(paths[0].text().view());

  return information;
}

fn Server::hover(const JsonValue *id, const JsonValue *params) throws -> bool
{
  let const request = request_positioned_document(params);
  if (!request.has_value()) return send_result(id, "null");
  let *document = request->document;
  select_document_mood(*document);
  let const symbol = symbol_at(*document, request->position);
  if (!symbol.has_value()) return send_result(id, "null");
  let const is_command = symbol->role == highlight_role::resolved_command ||
                         symbol->role == highlight_role::partial_command ||
                         symbol->role == highlight_role::unknown_command;
  if (!is_command) return send_result(id, "null");
  if (definition_of(*document, *symbol).has_value())
    return send_result(id, "null");
  let const information = command_information(symbol->text.view());
  if (!information.has_value()) return send_result(id, "null");
  let response = String{"{\"contents\":{\"kind\":\"plaintext\",\"value\":"};
  append_json_string(response, information->view());
  response.append("},\"range\":");
  append_protocol_range(response, *document, symbol->start, symbol->end,
                        m_encoding);
  response.push('}');

  return send_result(id, response.view());
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

  for (usize line = 0; line < document->line_starts.count(); line++) {
    let const line_start = document->line_starts[line];
    let line_end = document->normalized_source.count();
    if (line + 1 < document->line_starts.count())
      line_end = document->line_starts[line + 1] - 1;
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
      let const[type, modifiers] = semantic_style(span.role);
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
  case request_method::CodeAction: return code_actions(id, params);
  case request_method::Definition: return definition(id, params);
  case request_method::Hover: return hover(id, params);
  case request_method::SemanticTokens: return semantic_tokens(id, params);
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
