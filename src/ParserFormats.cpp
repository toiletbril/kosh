/*
 *    This file is a part of the Koshka shell, (c) toiletbril, 2026
 *    See the top-level LICENSE file for the licensing information.
 *
 * This file detects host document formats and extracts embedded shell
 * fragments for analysis and formatting. It maps positions between host and
 * shell source, handles indentation and JSON string codecs, and applies
 * formatted replacements to the host document. Format-neutral detection,
 * mapping, codecs, and dispatch stay here. Each source under formats
 * implements one host format.
 */

#include "ParserFormats.hpp"

#include "MimicMood.hpp"
#include "Path.hpp"

namespace koshka {

ParserFormat::ParserFormat() = default;

#define DEFINE_PARSER_FORMAT(parser, format_kind, parse_function)              \
  parser::parser() = default;                                                  \
  pure fn parser::kind() const wontthrow -> parser_format_kind                 \
  {                                                                            \
    return parser_format_kind::format_kind;                                    \
  }                                                                            \
  fn parser::parse(const parser_format_input &input,                           \
                   parsed_format_document &document) const throws -> void      \
  {                                                                            \
    parse_function(input, document);                                           \
  }

DEFINE_PARSER_FORMAT(GithubActionsFormat, GithubActions,
                     parse_github_actions_format);
DEFINE_PARSER_FORMAT(GiteaActionsFormat, GiteaActions,
                     parse_gitea_actions_format);
DEFINE_PARSER_FORMAT(ForgejoActionsFormat, ForgejoActions,
                     parse_forgejo_actions_format);
DEFINE_PARSER_FORMAT(GitlabCiFormat, GitlabCi, parse_gitlab_ci_format);
DEFINE_PARSER_FORMAT(AnsibleFormat, Ansible, parse_ansible_format);
DEFINE_PARSER_FORMAT(DockerfileFormat, Dockerfile, parse_dockerfile_format);
DEFINE_PARSER_FORMAT(ComposeFormat, Compose, parse_compose_format);
DEFINE_PARSER_FORMAT(MarkdownFormat, Markdown, parse_markdown_format);
DEFINE_PARSER_FORMAT(MakefileFormat, Makefile, parse_makefile_format);
DEFINE_PARSER_FORMAT(CloudBuildFormat, CloudBuild, parse_cloud_build_format);
DEFINE_PARSER_FORMAT(CircleCiFormat, CircleCi, parse_circle_ci_format);
DEFINE_PARSER_FORMAT(AzurePipelinesFormat, AzurePipelines,
                     parse_azure_pipelines_format);
DEFINE_PARSER_FORMAT(BitbucketPipelinesFormat, BitbucketPipelines,
                     parse_bitbucket_pipelines_format);
DEFINE_PARSER_FORMAT(BuildkiteFormat, Buildkite, parse_buildkite_format);
DEFINE_PARSER_FORMAT(TravisCiFormat, TravisCi, parse_travis_ci_format);
DEFINE_PARSER_FORMAT(KubernetesFormat, Kubernetes, parse_kubernetes_format);
DEFINE_PARSER_FORMAT(PackageJsonFormat, PackageJson, parse_package_json_format);
DEFINE_PARSER_FORMAT(DroneFormat, Drone, parse_drone_format);
DEFINE_PARSER_FORMAT(WoodpeckerFormat, Woodpecker, parse_woodpecker_format);
DEFINE_PARSER_FORMAT(TaskfileFormat, Taskfile, parse_taskfile_format);
DEFINE_PARSER_FORMAT(JustfileFormat, Justfile, parse_justfile_format);
DEFINE_PARSER_FORMAT(RpmSpecFormat, RpmSpec, parse_rpm_spec_format);
DEFINE_PARSER_FORMAT(VscodeTasksFormat, VscodeTasks, parse_vscode_tasks_format);
DEFINE_PARSER_FORMAT(DevContainerFormat, DevContainer,
                     parse_dev_container_format);

pure fn parser_format_has_substring(StringView source,
                                    StringView wanted) wontthrow -> bool
{
  return source.find_substring(wanted).has_value();
}

pure fn parser_format_filename(StringView path) wontthrow -> StringView
{
  usize start = 0;
  for (usize position = 0; position < path.length; position++)
    if (path[position] == '/' || path[position] == '\\') start = position + 1;

  return path.substring(start);
}

pure fn parser_format_ascii_equal(StringView left, StringView right) wontthrow
    -> bool
{
  if (left.length != right.length) return false;

  for (usize position = 0; position < left.length; position++) {
    let left_byte = left[position];
    let right_byte = right[position];
    if (left_byte == right_byte) continue;
    if (left_byte >= 'A' && left_byte <= 'Z') left_byte += 'a' - 'A';
    if (right_byte >= 'A' && right_byte <= 'Z') right_byte += 'a' - 'A';
    if (left_byte != right_byte) return false;
  }

  return true;
}

static pure fn path_ends_with(StringView path, StringView suffix) wontthrow
    -> bool
{
  return suffix.length <= path.length &&
         parser_format_ascii_equal(path.substring(path.length - suffix.length),
                                   suffix);
}

static pure fn path_holds(StringView path, StringView part) wontthrow -> bool
{
  if (part.length > path.length) return false;

  for (usize position = 0; position + part.length <= path.length; position++) {
    if (parser_format_ascii_equal(
            path.substring_of_length(position, part.length), part))
      return true;
  }

  return false;
}

static pure fn known_host_extension(StringView path) wontthrow -> bool
{
  static const StringView EXTENSIONS[] = {
      ".json", ".jsonc", ".markdown", ".md",   ".mk",
      ".nix",  ".spec",  ".tf",       ".yaml", ".yml",
  };

  for (let const extension : EXTENSIONS)
    if (path_ends_with(path, extension)) return true;

  return false;
}

static pure fn detect_format_kind(const parser_format_input &input) throws
    -> parser_format_kind
{
  let path = input.path.has_value() ? *input.path : StringView{};
  let const filename = parser_format_filename(path);
  let const source = input.source;
  let const language_id =
      input.language_id.has_value() ? *input.language_id : StringView{};

  if (path_holds(path, "/.github/workflows/") ||
      parser_format_ascii_equal(filename, "action.yml") ||
      parser_format_ascii_equal(filename, "action.yaml"))
    return parser_format_kind::GithubActions;
  if (path_holds(path, "/.gitea/workflows/"))
    return parser_format_kind::GiteaActions;
  if (path_holds(path, "/.forgejo/workflows/"))
    return parser_format_kind::ForgejoActions;
  if (parser_format_ascii_equal(filename, ".gitlab-ci.yml"))
    return parser_format_kind::GitlabCi;
  if (parser_format_ascii_equal(filename, ".drone.yml"))
    return parser_format_kind::Drone;
  if (path_holds(path, "/.woodpecker/") ||
      parser_format_ascii_equal(filename, ".woodpecker.yml") ||
      parser_format_ascii_equal(filename, ".woodpecker.yaml"))
    return parser_format_kind::Woodpecker;
  if (path_holds(path, "/.circleci/") &&
      (path_ends_with(path, ".yml") || path_ends_with(path, ".yaml")))
    return parser_format_kind::CircleCi;
  if (parser_format_ascii_equal(filename, "azure-pipelines.yml") ||
      parser_format_ascii_equal(filename, "azure-pipelines.yaml"))
    return parser_format_kind::AzurePipelines;
  if (parser_format_ascii_equal(filename, "bitbucket-pipelines.yml") ||
      parser_format_ascii_equal(filename, "bitbucket-pipelines.yaml"))
    return parser_format_kind::BitbucketPipelines;
  if (path_holds(path, "/.buildkite/") &&
      (path_ends_with(path, ".yml") || path_ends_with(path, ".yaml")))
    return parser_format_kind::Buildkite;
  if (parser_format_ascii_equal(filename, ".travis.yml"))
    return parser_format_kind::TravisCi;
  if (parser_format_ascii_equal(filename, "cloudbuild.yml") ||
      parser_format_ascii_equal(filename, "cloudbuild.yaml"))
    return parser_format_kind::CloudBuild;
  if (parser_format_ascii_equal(filename, "compose.yml") ||
      parser_format_ascii_equal(filename, "compose.yaml") ||
      parser_format_ascii_equal(filename, "docker-compose.yml") ||
      parser_format_ascii_equal(filename, "docker-compose.yaml"))
    return parser_format_kind::Compose;
  if (parser_format_ascii_equal(filename, "taskfile.yml") ||
      parser_format_ascii_equal(filename, "taskfile.yaml") ||
      parser_format_ascii_equal(filename, "taskfile.dist.yml") ||
      parser_format_ascii_equal(filename, "taskfile.dist.yaml"))
    return parser_format_kind::Taskfile;
  if (parser_format_ascii_equal(filename, "dockerfile") ||
      parser_format_ascii_equal(filename, "containerfile") ||
      path_ends_with(filename, ".dockerfile") ||
      path_ends_with(filename, ".containerfile"))
    return parser_format_kind::Dockerfile;
  if (parser_format_ascii_equal(filename, "makefile") ||
      parser_format_ascii_equal(filename, "gnumakefile") ||
      path_ends_with(filename, ".mk"))
    return parser_format_kind::Makefile;
  if (parser_format_ascii_equal(filename, "justfile") ||
      parser_format_ascii_equal(filename, ".justfile"))
    return parser_format_kind::Justfile;
  if (parser_format_ascii_equal(filename, "package.json"))
    return parser_format_kind::PackageJson;
  if (path_ends_with(path, "/.vscode/tasks.json"))
    return parser_format_kind::VscodeTasks;
  if (parser_format_ascii_equal(filename, "devcontainer.json") &&
      (path_holds(path, "/.devcontainer/") ||
       path_ends_with(path, "/.devcontainer.json")))
    return parser_format_kind::DevContainer;
  if (path_ends_with(path, ".spec")) return parser_format_kind::RpmSpec;
  if (path_ends_with(path, ".md") || path_ends_with(path, ".markdown") ||
      parser_format_ascii_equal(filename, "readme") ||
      parser_format_ascii_equal(language_id, "markdown"))
    return parser_format_kind::Markdown;

  if (path_ends_with(path, ".yml") || path_ends_with(path, ".yaml") ||
      parser_format_ascii_equal(language_id, "yaml"))
  {
    if (parser_format_has_substring(source, "apiVersion:") &&
        parser_format_has_substring(source, "kind:"))
      return parser_format_kind::Kubernetes;
    if (parser_format_has_substring(source, "runs-on:") &&
        parser_format_has_substring(source, "steps:"))
      return parser_format_kind::GithubActions;
    if (parser_format_has_substring(source, "tasks:") &&
        parser_format_has_substring(source, "version:"))
      return parser_format_kind::Taskfile;
    if (parser_format_has_substring(source, "hosts:") &&
        parser_format_has_substring(source, "tasks:"))
      return parser_format_kind::Ansible;
    if (parser_format_has_substring(source, "services:") &&
        (parser_format_has_substring(source, "image:") ||
         parser_format_has_substring(source, "build:")))
      return parser_format_kind::Compose;
    if (parser_format_has_substring(source, "pipelines:") &&
        parser_format_has_substring(source, "script:"))
      return parser_format_kind::BitbucketPipelines;
    if (parser_format_has_substring(source, "jobs:") &&
        parser_format_has_substring(source, "script:"))
      return parser_format_kind::GitlabCi;

    return parser_format_kind::UnknownHost;
  }

  if (parser_format_ascii_equal(language_id, "dockerfile"))
    return parser_format_kind::Dockerfile;
  if (parser_format_ascii_equal(language_id, "makefile"))
    return parser_format_kind::Makefile;
  if (parser_format_ascii_equal(language_id, "json") ||
      parser_format_ascii_equal(language_id, "jsonc"))
    return parser_format_kind::UnknownHost;

  if (known_host_extension(path) ||
      parser_format_ascii_equal(filename, "jenkinsfile"))
    return parser_format_kind::UnknownHost;

  if (path.is_empty() || !language_id.is_empty() ||
      Path{path}.is_shell_source(source))
  {
    return parser_format_kind::Shell;
  }

  return parser_format_kind::UnknownHost;
}

static fn make_analysis_source(StringView host_source, usize host_start,
                               usize host_end) throws -> String
{
  let analysis_source = String{heap_allocator()};
  analysis_source.reserve(host_end);

  for (usize position = 0; position < host_end; position++) {
    let byte = host_source[position];
    if (byte != '\n' && (position < host_start || position >= host_end))
      byte = ' ';
    analysis_source.push(byte);
  }

  return analysis_source;
}

fn parser_format_add_fragment(parsed_format_document &document,
                              StringView host_source, usize host_start,
                              usize host_end, mimic_mood mood,
                              parser_format_codec codec, usize indent_length,
                              Maybe<String> prepared_analysis_source) throws
    -> void
{
  if (host_start >= host_end || host_end > host_source.length) return;

  let fragment = parser_format_fragment{};
  fragment.analysis_source =
      prepared_analysis_source.has_value()
          ? prepared_analysis_source.take()
          : make_analysis_source(host_source, host_start, host_end);
  fragment.shell_source = String{
      host_source.substring_of_length(host_start, host_end - host_start)};
  fragment.mood = mood;
  fragment.codec = codec;
  fragment.should_silence_unresolved_commands =
      parser_format_should_silence_unresolved_commands(document.kind);
  fragment.host_start = host_start;
  fragment.host_end = host_end;
  fragment.indent_length = indent_length;
  document.fragments.push(steal(fragment));
}

fn parser_format_add_indented_fragment(parsed_format_document &document,
                                       StringView host_source, usize host_start,
                                       usize host_end, usize indent_length,
                                       mimic_mood mood) throws -> void
{
  parser_format_add_fragment(document, host_source, host_start, host_end, mood,
                             parser_format_codec::Indented, indent_length);
  if (document.fragments.is_empty()) return;

  let &fragment = document.fragments.back();
  let deindented = String{heap_allocator()};
  usize position = host_start;

  while (position < host_end) {
    let const line_start = position;
    let line = host_source.next_line(position);
    if (line_start + line.length > host_end)
      line = host_source.substring_of_length(line_start, host_end - line_start);
    let removed = usize{0};
    while (removed < line.length && removed < indent_length &&
           line[removed] == ' ')
      removed++;
    deindented.append(line.substring(removed));
    if (position <= host_end && position > line_start + line.length)
      deindented.push('\n');
  }
  fragment.shell_source = steal(deindented);
}

static pure fn yaml_key_match(StringView line, const StringView *keys,
                              usize key_count,
                              usize &content_position) wontthrow -> bool
{
  usize position = 0;
  while (position < line.length &&
         (line[position] == ' ' || line[position] == '\t'))
    position++;
  if (position < line.length && line[position] == '-') {
    position++;
    while (position < line.length && line[position] == ' ')
      position++;
  }
  if (position >= line.length) return false;

  for (usize key_index = 0; key_index < key_count; key_index++) {
    let const key = keys[key_index];
    if (key.is_empty()) continue;
    if (line[position] != key[0]) continue;
    if (position + key.length >= line.length) continue;
    if (line.substring_of_length(position, key.length) != key) continue;
    if (line[position + key.length] != ':') continue;
    content_position = position + key.length + 1;
    while (content_position < line.length &&
           (line[content_position] == ' ' || line[content_position] == '\t'))
      content_position++;
    return true;
  }

  return false;
}

static pure fn leading_spaces(StringView line) wontthrow -> usize
{
  usize count = 0;
  while (count < line.length && line[count] == ' ')
    count++;
  return count;
}

static pure fn mood_near(StringView source, usize position,
                         mimic_mood fallback) wontthrow -> mimic_mood
{
  let const start = position > 512 ? position - 512 : 0;
  let const nearby = source.substring_of_length(start, position - start);
  if (parser_format_has_substring(nearby, "shell: bash") ||
      parser_format_has_substring(nearby, "executable: /bin/bash") ||
      parser_format_has_substring(nearby, "interpreter: bash"))
    return mimic_mood::Bash;
  if (parser_format_has_substring(nearby, "shell: kosh") ||
      parser_format_has_substring(nearby, "executable: kosh"))
    return mimic_mood::Default;
  if (parser_format_has_substring(nearby, "shell: sh") ||
      parser_format_has_substring(nearby, "executable: /bin/sh"))
    return mimic_mood::Posix;

  return fallback;
}

static pure fn yaml_key_position(StringView line) wontthrow -> usize
{
  usize position = leading_spaces(line);
  if (position < line.length && line[position] == '-') {
    position++;
    while (position < line.length && line[position] == ' ')
      position++;
  }

  return position;
}

static pure fn yaml_line_value(StringView line, StringView key) wontthrow
    -> Maybe<StringView>
{
  let const position = yaml_key_position(line);
  if (position + key.length >= line.length ||
      line.substring_of_length(position, key.length) != key ||
      line[position + key.length] != ':')
    return None;

  return line.substring(position + key.length + 1).trim_blanks();
}

static pure fn previous_line(StringView source, usize &position) wontthrow
    -> StringView
{
  if (position == 0) return StringView{};
  usize end = position;
  if (end > 0 && source[end - 1] == '\n') end--;
  let start = end;
  while (start > 0 && source[start - 1] != '\n')
    start--;
  position = start;

  return source.substring_of_length(start, end - start);
}

static pure fn workflow_shell_mood(StringView value) wontthrow
    -> Maybe<mimic_mood>
{
  if (!value.is_empty() && (value[0] == '\'' || value[0] == '"'))
    value = value.substring(1);
  if (value.is_empty()) return None;

  switch (value[0]) {
  case 'b':
    if (value.starts_with("bash")) return mimic_mood::Bash;
    break;
  case 'd':
    if (value.starts_with("dash")) return mimic_mood::Posix;
    break;
  case 's':
    if (value.starts_with("sh")) return mimic_mood::Posix;
    break;
  case 'k':
    if (value.starts_with("kosh")) return mimic_mood::Default;
    break;
  default: break;
  }

  return None;
}

static pure fn selected_workflow_mood(StringView source, usize line_start,
                                      StringView line,
                                      mimic_mood fallback) wontthrow
    -> Maybe<mimic_mood>
{
  let const current_indent = leading_spaces(line);
  let scan_position = line_start;
  while (scan_position > 0) {
    let const previous = previous_line(source, scan_position);
    if (previous.trim_blanks().is_empty()) continue;
    let const previous_indent = leading_spaces(previous);
    if (previous_indent == current_indent &&
        previous_indent < previous.length && previous[previous_indent] == '-')
      break;
    if (let const shell = yaml_line_value(previous, "shell"); shell.has_value())
      return workflow_shell_mood(*shell);
    if (previous_indent < current_indent) break;
  }

  scan_position = line_start;
  while (scan_position > 0) {
    let const previous = previous_line(source, scan_position);
    let const runner = yaml_line_value(previous, "runs-on");
    if (!runner.has_value()) continue;
    if (parser_format_has_substring(*runner, "windows")) return None;
    break;
  }

  return fallback;
}

static fn add_yaml_inline_fragment(parsed_format_document &document,
                                   StringView source, usize start, usize end,
                                   mimic_mood mood) throws -> void
{
  if (start >= end) return;
  let codec = parser_format_codec::Direct;
  if ((source[start] == '\'' && source[end - 1] == '\'') ||
      (source[start] == '"' && source[end - 1] == '"'))
  {
    start++;
    end--;
  }
  parser_format_add_fragment(document, source, start, end, mood, codec);
}

fn parser_format_extract_yaml_keys(parsed_format_document &document,
                                   StringView source, const StringView *keys,
                                   usize key_count, mimic_mood default_mood,
                                   bool should_select_workflow_shell) throws
    -> void
{
  usize position = 0;
  while (position < source.length) {
    let const line_start = position;
    let const line = source.next_line(position);
    usize content_position = 0;
    if (!yaml_key_match(line, keys, key_count, content_position)) continue;
    let const mood =
        should_select_workflow_shell
            ? selected_workflow_mood(source, line_start, line, default_mood)
            : Maybe<mimic_mood>{mood_near(source, line_start, default_mood)};
    if (!mood.has_value()) continue;
    if (content_position < line.length &&
        (line[content_position] == '|' || line[content_position] == '>'))
    {
      let block_position = position;
      usize block_start = position;
      usize block_end = position;
      usize content_indent = 0;
      bool has_block_start = false;
      while (block_position < source.length) {
        let const next_start = block_position;
        let const next_line = source.next_line(block_position);
        if (!next_line.trim_blanks().is_empty()) {
          let const indent = leading_spaces(next_line);
          if (indent <= leading_spaces(line)) break;
          if (content_indent == 0) content_indent = indent;
        }
        block_end = block_position;
        if (!has_block_start) {
          block_start = next_start;
          has_block_start = true;
        }
      }
      if (content_indent > 0)
        parser_format_add_indented_fragment(document, source, block_start,
                                            block_end, content_indent, *mood);
      continue;
    }

    if (content_position < line.length) {
      add_yaml_inline_fragment(document, source, line_start + content_position,
                               line_start + line.length, *mood);
      continue;
    }

    let sequence_position = position;
    while (sequence_position < source.length) {
      let const item_start = sequence_position;
      let const item_line = source.next_line(sequence_position);
      if (item_line.trim_blanks().is_empty()) continue;
      if (leading_spaces(item_line) <= leading_spaces(line)) break;
      let item_content = leading_spaces(item_line);
      if (item_content >= item_line.length || item_line[item_content] != '-')
        continue;
      item_content++;
      while (item_content < item_line.length && item_line[item_content] == ' ')
        item_content++;
      add_yaml_inline_fragment(document, source, item_start + item_content,
                               item_start + item_line.length, *mood);
    }
  }
}

fn parser_format_add_json_fragment(parsed_format_document &document,
                                   StringView source, usize start, usize end,
                                   mimic_mood mood) throws -> void
{
  if (start >= end) return;
  parser_format_add_fragment(document, source, start, end, mood,
                             parser_format_codec::JsonString);
  if (document.fragments.is_empty()) return;
  let &fragment = document.fragments.back();
  let decoded = String{heap_allocator()};
  usize position = start;
  while (position < end) {
    let byte = source[position++];
    if (byte != '\\' || position >= end) {
      decoded.push(byte);
      continue;
    }
    let const escaped = source[position++];
    switch (escaped) {
    case 'n': decoded.push('\n'); break;
    case 'r': decoded.push('\r'); break;
    case 't': decoded.push('\t'); break;
    case '"': decoded.push('"'); break;
    case '\\': decoded.push('\\'); break;
    default:
      decoded.push('\\');
      decoded.push(escaped);
      break;
    }
  }
  fragment.shell_source = steal(decoded);
  let masked = String{heap_allocator()};
  masked.reserve(end);
  for (usize host_position = 0; host_position < end; host_position++) {
    let byte = source[host_position];
    if (host_position < start || host_position >= end)
      byte = byte == '\n' ? '\n' : ' ';
    else if (byte == '\\' && host_position + 1 < end)
      byte = ' ';
    masked.push(byte);
  }
  fragment.analysis_source = steal(masked);
}

fn parser_format_extract_json_keys(parsed_format_document &document,
                                   StringView source,
                                   const parser_format_json_key *keys,
                                   usize key_count,
                                   mimic_mood default_mood) throws -> void
{
  usize position = 0;
  while (position < source.length) {
    if (source[position] != '"') {
      position++;
      continue;
    }
    let const key_start = ++position;
    while (position < source.length && source[position] != '"') {
      if (source[position] == '\\' && position + 1 < source.length) position++;
      position++;
    }
    if (position >= source.length) break;
    let const key = source.substring_of_length(key_start, position - key_start);
    position++;
    if (key.is_empty()) continue;
    while (position < source.length &&
           (source[position] == ' ' || source[position] == '\t' ||
            source[position] == '\n' || source[position] == '\r'))
      position++;
    if (position >= source.length || source[position] != ':') continue;
    position++;

    const parser_format_json_key *matched_key = nullptr;
    for (usize key_index = 0; key_index < key_count; key_index++) {
      if (keys[key_index].name.is_empty()) continue;
      if (key[0] != keys[key_index].name[0]) continue;
      if (key != keys[key_index].name) continue;
      matched_key = &keys[key_index];
      break;
    }
    if (matched_key == nullptr) continue;
    while (position < source.length &&
           (source[position] == ' ' || source[position] == '\t' ||
            source[position] == '\n' || source[position] == '\r'))
      position++;
    if (position >= source.length || source[position] != '"') continue;
    let const value_start = ++position;
    while (position < source.length && source[position] != '"') {
      if (source[position] == '\\' && position + 1 < source.length) position++;
      position++;
    }
    let const fragment_count = document.fragments.count();
    parser_format_add_json_fragment(document, source, value_start, position,
                                    default_mood);
    if (document.fragments.count() != fragment_count)
      document.fragments.back().should_silence_unresolved_commands =
          matched_key->should_silence_unresolved_commands;
    if (position < source.length) position++;
  }
}

pure fn parser_format_fragment_at(const parsed_format_document &document,
                                  usize host_position) wontthrow -> Maybe<usize>
{
  usize lower = 0;
  usize upper = document.fragments.count();
  while (lower < upper) {
    let const middle = lower + (upper - lower) / 2;
    let const &fragment = document.fragments[middle];
    if (host_position < fragment.host_start) {
      upper = middle;
    } else if (host_position > fragment.host_end ||
               (host_position == fragment.host_end &&
                !fragment.should_select_end))
    {
      lower = middle + 1;
    } else {
      return middle;
    }
  }

  return None;
}

fn parser_format_encode(const parser_format_fragment &fragment,
                        StringView replacement) throws -> Maybe<String>
{
  let encoded = String{heap_allocator()};
  switch (fragment.codec) {
  case parser_format_codec::Direct: encoded.append(replacement); return encoded;
  case parser_format_codec::JsonString:
    for (usize position = 0; position < replacement.length; position++) {
      switch (replacement[position]) {
      case '\\': encoded.append("\\\\"); break;
      case '"': encoded.append("\\\""); break;
      case '\n': encoded.append("\\n"); break;
      case '\r': encoded.append("\\r"); break;
      case '\t': encoded.append("\\t"); break;
      default: encoded.push(replacement[position]); break;
      }
    }
    return encoded;
  case parser_format_codec::Indented: break;
  }

  usize position = 0;
  while (position < replacement.length) {
    encoded.append_repeated(' ', fragment.indent_length);
    let const line_start = position;
    let const line = replacement.next_line(position);
    encoded.append(line);
    if (position > line_start + line.length) encoded.push('\n');
  }

  return encoded;
}

fn parser_format_apply_replacements(
    StringView source, ArrayList<parser_format_replacement> replacements) throws
    -> Maybe<String>
{
  replacements.sort([](const parser_format_replacement &left,
                       const parser_format_replacement &right) {
    return left.start_position < right.start_position;
  });
  let result = String{heap_allocator()};
  usize position = 0;
  for (let const &replacement : replacements) {
    if (replacement.start_position < position ||
        replacement.end_position < replacement.start_position ||
        replacement.end_position > source.length)
      return None;
    result.append(source.substring_of_length(
        position, replacement.start_position - position));
    result.append(replacement.replacement.view());
    position = replacement.end_position;
  }
  result.append(source.substring(position));

  return result;
}

fn parser_format_analysis_source(const parsed_format_document &document,
                                 StringView host_source) throws -> String
{
  if (!document.is_host_format) return String{host_source};

  let result = String{heap_allocator()};
  result.reserve(host_source.length);
  usize fragment_index = 0;
  for (usize position = 0; position < host_source.length; position++) {
    while (fragment_index < document.fragments.count() &&
           position >= document.fragments[fragment_index].host_end)
      fragment_index++;
    if (fragment_index < document.fragments.count() &&
        document.fragments[fragment_index].contains(position))
      result.push(document.fragments[fragment_index].analysis_source[position]);
    else
      result.push(host_source[position] == '\n' ? '\n' : ' ');
  }

  return result;
}

pure fn parser_format_should_silence_unresolved_commands(
    parser_format_kind kind) wontthrow -> bool
{
  switch (kind) {
  case parser_format_kind::Shell:
  case parser_format_kind::UnknownHost:
  case parser_format_kind::Markdown:
  case parser_format_kind::Makefile:
  case parser_format_kind::Taskfile:
  case parser_format_kind::Justfile:
  case parser_format_kind::VscodeTasks: return false;
  case parser_format_kind::GithubActions:
  case parser_format_kind::GiteaActions:
  case parser_format_kind::ForgejoActions:
  case parser_format_kind::GitlabCi:
  case parser_format_kind::Ansible:
  case parser_format_kind::Dockerfile:
  case parser_format_kind::Compose:
  case parser_format_kind::CloudBuild:
  case parser_format_kind::CircleCi:
  case parser_format_kind::AzurePipelines:
  case parser_format_kind::BitbucketPipelines:
  case parser_format_kind::Buildkite:
  case parser_format_kind::TravisCi:
  case parser_format_kind::Kubernetes:
  case parser_format_kind::PackageJson:
  case parser_format_kind::Drone:
  case parser_format_kind::Woodpecker:
  case parser_format_kind::RpmSpec:
  case parser_format_kind::DevContainer: return true;
  }

  return false;
}

fn parse_format_document(const parser_format_input &input) throws
    -> parsed_format_document
{
  let document = parsed_format_document{};
  document.kind = detect_format_kind(input);
  document.is_host_format = document.kind != parser_format_kind::Shell;

  switch (document.kind) {
  case parser_format_kind::Shell:
  case parser_format_kind::UnknownHost: break; PARSER_FORMAT_SWITCH_CASES();
  }

  return document;
}

} /* namespace koshka */
