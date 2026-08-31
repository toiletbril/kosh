#pragma once

#include "ArrayList.hpp"
#include "Maybe.hpp"
#include "MimicMood.hpp"
#include "String.hpp"
#include "StringView.hpp"

namespace koshka {

#define PARSER_FORMAT_KIND_ENTRIES(X)                                          \
  X(Shell, "shell scripts")                                                    \
  X(UnknownHost, "")                                                           \
  X(GithubActions, "GitHub Actions")                                           \
  X(GiteaActions, "Gitea Actions")                                             \
  X(ForgejoActions, "Forgejo Actions")                                         \
  X(GitlabCi, "GitLab CI")                                                     \
  X(Ansible, "Ansible")                                                        \
  X(Dockerfile, "Dockerfiles")                                                 \
  X(Compose, "Compose files")                                                  \
  X(Markdown, "Markdown")                                                      \
  X(Makefile, "Makefiles")                                                     \
  X(CloudBuild, "Google Cloud Build")                                          \
  X(CircleCi, "CircleCI")                                                      \
  X(AzurePipelines, "Azure Pipelines")                                         \
  X(BitbucketPipelines, "Bitbucket Pipelines")                                 \
  X(Buildkite, "Buildkite")                                                    \
  X(TravisCi, "Travis CI")                                                     \
  X(Kubernetes, "Kubernetes")                                                  \
  X(PackageJson, "package.json")                                               \
  X(Drone, "Drone")                                                            \
  X(Woodpecker, "Woodpecker")                                                  \
  X(Taskfile, "Taskfiles")                                                     \
  X(Justfile, "Justfiles")                                                     \
  X(RpmSpec, "RPM specs")                                                      \
  X(VscodeTasks, "VS Code tasks")                                              \
  X(DevContainer, "Dev Containers")

enum class parser_format_kind : u8
{
#define PARSER_FORMAT_KIND_ENUM(kind, label) kind,
  PARSER_FORMAT_KIND_ENTRIES(PARSER_FORMAT_KIND_ENUM)
#undef PARSER_FORMAT_KIND_ENUM
};

constexpr usize PARSER_FORMAT_KIND_COUNT =
    static_cast<usize>(parser_format_kind::DevContainer) + 1;

inline pure fn parser_format_kind_name(parser_format_kind kind) wontthrow
    -> StringView
{
  static constexpr StringView NAMES[PARSER_FORMAT_KIND_COUNT] = {
#define PARSER_FORMAT_KIND_NAME(kind, label) label,
      PARSER_FORMAT_KIND_ENTRIES(PARSER_FORMAT_KIND_NAME)
#undef PARSER_FORMAT_KIND_NAME
  };
  let const index = static_cast<usize>(kind);
  return index < PARSER_FORMAT_KIND_COUNT ? NAMES[index] : StringView{};
}

#undef PARSER_FORMAT_KIND_ENTRIES

enum class parser_format_codec : u8
{
  Direct,
  Indented,
  JsonString,
};

struct parser_format_json_key
{
  StringView name;
  bool should_silence_unresolved_commands;
};

struct parser_format_fragment
{
  String analysis_source{heap_allocator()};
  String shell_source{heap_allocator()};
  mimic_mood mood{mimic_mood::Posix};
  parser_format_codec codec{parser_format_codec::Direct};
  bool should_silence_unresolved_commands{false};
  bool should_select_end{false};
  usize host_start{0};
  usize host_end{0};
  usize indent_length{0};

  pure fn contains(usize host_position) const wontthrow -> bool
  {
    return host_position >= host_start && host_position < host_end;
  }
};

struct parsed_format_document
{
  parser_format_kind kind{parser_format_kind::Shell};
  bool is_host_format{false};
  ArrayList<parser_format_fragment> fragments{heap_allocator()};
};

struct parser_format_input
{
  StringView source;
  Maybe<StringView> path;
  Maybe<StringView> language_id;
};

struct parser_format_replacement
{
  usize start_position{0};
  usize end_position{0};
  String replacement{heap_allocator()};
};

class ParserFormat
{
public:
  pure virtual parser_format_kind kind() const wontthrow = 0;
  virtual fn parse(const parser_format_input &input,
                   parsed_format_document &document) const throws -> void = 0;
  virtual ~ParserFormat() = default;

protected:
  ParserFormat();
};

#define PARSER_FORMAT_CLASS(parser, format_kind)                               \
  class parser : public ParserFormat                                           \
  {                                                                            \
  public:                                                                      \
    parser();                                                                  \
    pure parser_format_kind kind() const wontthrow override;                   \
    fn parse(const parser_format_input &input,                                 \
             parsed_format_document &document) const throws -> void override;  \
  }

PARSER_FORMAT_CLASS(GithubActionsFormat, GithubActions);
PARSER_FORMAT_CLASS(GiteaActionsFormat, GiteaActions);
PARSER_FORMAT_CLASS(ForgejoActionsFormat, ForgejoActions);
PARSER_FORMAT_CLASS(GitlabCiFormat, GitlabCi);
PARSER_FORMAT_CLASS(AnsibleFormat, Ansible);
PARSER_FORMAT_CLASS(DockerfileFormat, Dockerfile);
PARSER_FORMAT_CLASS(ComposeFormat, Compose);
PARSER_FORMAT_CLASS(MarkdownFormat, Markdown);
PARSER_FORMAT_CLASS(MakefileFormat, Makefile);
PARSER_FORMAT_CLASS(CloudBuildFormat, CloudBuild);
PARSER_FORMAT_CLASS(CircleCiFormat, CircleCi);
PARSER_FORMAT_CLASS(AzurePipelinesFormat, AzurePipelines);
PARSER_FORMAT_CLASS(BitbucketPipelinesFormat, BitbucketPipelines);
PARSER_FORMAT_CLASS(BuildkiteFormat, Buildkite);
PARSER_FORMAT_CLASS(TravisCiFormat, TravisCi);
PARSER_FORMAT_CLASS(KubernetesFormat, Kubernetes);
PARSER_FORMAT_CLASS(PackageJsonFormat, PackageJson);
PARSER_FORMAT_CLASS(DroneFormat, Drone);
PARSER_FORMAT_CLASS(WoodpeckerFormat, Woodpecker);
PARSER_FORMAT_CLASS(TaskfileFormat, Taskfile);
PARSER_FORMAT_CLASS(JustfileFormat, Justfile);
PARSER_FORMAT_CLASS(RpmSpecFormat, RpmSpec);
PARSER_FORMAT_CLASS(VscodeTasksFormat, VscodeTasks);
PARSER_FORMAT_CLASS(DevContainerFormat, DevContainer);

#define PF_CASE(parser, format_kind)                                           \
  case parser_format_kind::format_kind: {                                      \
    parser format_parser;                                                      \
    format_parser.parse(input, document);                                      \
    break;                                                                     \
  }

#define PARSER_FORMAT_SWITCH_CASES()                                           \
  PF_CASE(GithubActionsFormat, GithubActions);                                 \
  PF_CASE(GiteaActionsFormat, GiteaActions);                                   \
  PF_CASE(ForgejoActionsFormat, ForgejoActions);                               \
  PF_CASE(GitlabCiFormat, GitlabCi);                                           \
  PF_CASE(AnsibleFormat, Ansible);                                             \
  PF_CASE(DockerfileFormat, Dockerfile);                                       \
  PF_CASE(ComposeFormat, Compose);                                             \
  PF_CASE(MarkdownFormat, Markdown);                                           \
  PF_CASE(MakefileFormat, Makefile);                                           \
  PF_CASE(CloudBuildFormat, CloudBuild);                                       \
  PF_CASE(CircleCiFormat, CircleCi);                                           \
  PF_CASE(AzurePipelinesFormat, AzurePipelines);                               \
  PF_CASE(BitbucketPipelinesFormat, BitbucketPipelines);                       \
  PF_CASE(BuildkiteFormat, Buildkite);                                         \
  PF_CASE(TravisCiFormat, TravisCi);                                           \
  PF_CASE(KubernetesFormat, Kubernetes);                                       \
  PF_CASE(PackageJsonFormat, PackageJson);                                     \
  PF_CASE(DroneFormat, Drone);                                                 \
  PF_CASE(WoodpeckerFormat, Woodpecker);                                       \
  PF_CASE(TaskfileFormat, Taskfile);                                           \
  PF_CASE(JustfileFormat, Justfile);                                           \
  PF_CASE(RpmSpecFormat, RpmSpec);                                             \
  PF_CASE(VscodeTasksFormat, VscodeTasks);                                     \
  PF_CASE(DevContainerFormat, DevContainer)

fn parse_format_document(const parser_format_input &input) throws
    -> parsed_format_document;
pure fn parser_format_fragment_at(const parsed_format_document &document,
                                  usize host_position) wontthrow
    -> Maybe<usize>;
fn parser_format_encode(const parser_format_fragment &fragment,
                        StringView replacement) throws -> Maybe<String>;
fn parser_format_apply_replacements(
    StringView source, ArrayList<parser_format_replacement> replacements) throws
    -> Maybe<String>;
fn parser_format_analysis_source(const parsed_format_document &document,
                                 StringView host_source) throws -> String;
pure fn parser_format_should_silence_unresolved_commands(
    parser_format_kind kind) wontthrow -> bool;

pure fn parser_format_has_substring(StringView source,
                                    StringView wanted) wontthrow -> bool;
pure fn parser_format_filename(StringView path) wontthrow -> StringView;
pure fn parser_format_ascii_equal(StringView left, StringView right) wontthrow
    -> bool;
fn parser_format_add_fragment(
    parsed_format_document &document, StringView host_source, usize host_start,
    usize host_end, mimic_mood mood,
    parser_format_codec codec = parser_format_codec::Direct,
    usize indent_length = 0,
    Maybe<String> prepared_analysis_source = None) throws -> void;
fn parser_format_add_indented_fragment(parsed_format_document &document,
                                       StringView host_source, usize host_start,
                                       usize host_end, usize indent_length,
                                       mimic_mood mood) throws -> void;
fn parser_format_extract_yaml_keys(
    parsed_format_document &document, StringView source, const StringView *keys,
    usize key_count, mimic_mood default_mood,
    bool should_select_workflow_shell = false) throws -> void;
fn parser_format_extract_json_keys(parsed_format_document &document,
                                   StringView source,
                                   const parser_format_json_key *keys,
                                   usize key_count,
                                   mimic_mood default_mood) throws -> void;
fn parser_format_add_json_fragment(parsed_format_document &document,
                                   StringView source, usize host_start,
                                   usize host_end, mimic_mood mood) throws
    -> void;

fn parse_github_actions_format(const parser_format_input &input,
                               parsed_format_document &document) throws -> void;
fn parse_gitea_actions_format(const parser_format_input &input,
                              parsed_format_document &document) throws -> void;
fn parse_forgejo_actions_format(const parser_format_input &input,
                                parsed_format_document &document) throws
    -> void;
fn parse_gitlab_ci_format(const parser_format_input &input,
                          parsed_format_document &document) throws -> void;
fn parse_ansible_format(const parser_format_input &input,
                        parsed_format_document &document) throws -> void;
fn parse_dockerfile_format(const parser_format_input &input,
                           parsed_format_document &document) throws -> void;
fn parse_compose_format(const parser_format_input &input,
                        parsed_format_document &document) throws -> void;
fn parse_markdown_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void;
fn parse_makefile_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void;
fn parse_cloud_build_format(const parser_format_input &input,
                            parsed_format_document &document) throws -> void;
fn parse_circle_ci_format(const parser_format_input &input,
                          parsed_format_document &document) throws -> void;
fn parse_azure_pipelines_format(const parser_format_input &input,
                                parsed_format_document &document) throws
    -> void;
fn parse_bitbucket_pipelines_format(const parser_format_input &input,
                                    parsed_format_document &document) throws
    -> void;
fn parse_buildkite_format(const parser_format_input &input,
                          parsed_format_document &document) throws -> void;
fn parse_travis_ci_format(const parser_format_input &input,
                          parsed_format_document &document) throws -> void;
fn parse_kubernetes_format(const parser_format_input &input,
                           parsed_format_document &document) throws -> void;
fn parse_package_json_format(const parser_format_input &input,
                             parsed_format_document &document) throws -> void;
fn parse_drone_format(const parser_format_input &input,
                      parsed_format_document &document) throws -> void;
fn parse_woodpecker_format(const parser_format_input &input,
                           parsed_format_document &document) throws -> void;
fn parse_taskfile_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void;
fn parse_justfile_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void;
fn parse_rpm_spec_format(const parser_format_input &input,
                         parsed_format_document &document) throws -> void;
fn parse_vscode_tasks_format(const parser_format_input &input,
                             parsed_format_document &document) throws -> void;
fn parse_dev_container_format(const parser_format_input &input,
                              parsed_format_document &document) throws -> void;

} /* namespace koshka */
