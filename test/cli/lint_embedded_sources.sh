unset KOSH_FLAGS
BIN=$(cd "$(dirname "$BIN")" && pwd -P)/$(basename "$BIN")
start_directory=$(pwd -P)
root=$TEST_TEMP_DIRECTORY/lint-embedded-sources
mkdir -p "$root/.github/workflows" "$root/.gitea/workflows" \
  "$root/.forgejo/workflows" "$root/.circleci" "$root/.buildkite" \
  "$root/.woodpecker" "$root/.vscode" "$root/.devcontainer"
root=$(cd "$root" && pwd -P)
trap 'cd "$start_directory" && [ -n "$root" ] && /bin/rm -rf "$root"' EXIT

cat > "$root/.github/workflows/check.yml" <<'EOF'
name: check
on: push
jobs:
  lint:
    runs-on: ubuntu-latest
    steps:
      - shell: bash
        run: |
          printf '%s\n' "$GITHUB_EMBEDDED"
          awk 'BEGIN { print "$10" }'
      - shell: pwsh
        run: |
          Write-Output "$POWERSHELL_HOST"
      - run: target-only-ci-command
EOF

cat > "$root/README.md" <<'EOF'
# Example

The prose contains $MARKDOWN_PROSE.

```bash
printf '%s\n' "$MARKDOWN_EMBEDDED"
```
EOF

cat > "$root/Containerfile" <<'EOF'
FROM alpine
SHELL ["/bin/bash", "-c"]
RUN printf '%s\n' "$CONTAINER_EMBEDDED"
EOF

cat > "$root/Makefile" <<'EOF'
SHELL := /bin/bash
all:
	printf '%s\n' "$MAKE_EMBEDDED"
EOF

cat > "$root/package.json" <<'EOF'
{
  "name": "embedded-test",
  "scripts": {
    "check": "printf '%s\\n' \"$PACKAGE_EMBEDDED\""
  }
}
EOF

cat > "$root/.gitea/workflows/check.yml" <<'EOF'
steps:
  - run: 'printf "%s\n" "$GITEA_EMBEDDED"'
EOF

cat > "$root/.forgejo/workflows/check.yml" <<'EOF'
steps:
  - run: 'printf "%s\n" "$FORGEJO_EMBEDDED"'
EOF

cat > "$root/.gitlab-ci.yml" <<'EOF'
job:
  script:
    - printf '%s\n' "$GITLAB_EMBEDDED"
EOF

cat > "$root/playbook.yml" <<'EOF'
- hosts: all
  tasks:
    - ansible.builtin.shell: printf '%s\n' "$ANSIBLE_EMBEDDED"
EOF

cat > "$root/compose.yml" <<'EOF'
services:
  app:
    image: alpine
    command: printf '%s\n' "$COMPOSE_EMBEDDED"
EOF

cat > "$root/cloudbuild.yaml" <<'EOF'
steps:
  - script: printf '%s\n' "$CLOUD_BUILD_EMBEDDED"
EOF

cat > "$root/.circleci/config.yml" <<'EOF'
version: 2.1
jobs:
  check:
    steps:
      - run: printf '%s\n' "$CIRCLE_EMBEDDED"
EOF

cat > "$root/azure-pipelines.yml" <<'EOF'
steps:
  - bash: printf '%s\n' "$AZURE_EMBEDDED"
EOF

cat > "$root/bitbucket-pipelines.yml" <<'EOF'
pipelines:
  default:
    - step:
        script:
          - printf '%s\n' "$BITBUCKET_EMBEDDED"
EOF

cat > "$root/.buildkite/pipeline.yml" <<'EOF'
steps:
  - command: printf '%s\n' "$BUILDKITE_EMBEDDED"
EOF

cat > "$root/.travis.yml" <<'EOF'
script: printf '%s\n' "$TRAVIS_EMBEDDED"
EOF

cat > "$root/pod.yml" <<'EOF'
apiVersion: v1
kind: Pod
spec:
  containers:
    - command: ["/bin/sh", "-c"]
      args: |
        printf '%s\n' "$KUBERNETES_EMBEDDED"
EOF

cat > "$root/.drone.yml" <<'EOF'
kind: pipeline
steps:
  - commands:
      - printf '%s\n' "$DRONE_EMBEDDED"
EOF

cat > "$root/.woodpecker/check.yml" <<'EOF'
steps:
  check:
    commands:
      - printf '%s\n' "$WOODPECKER_EMBEDDED"
EOF

cat > "$root/Taskfile.yml" <<'EOF'
version: '3'
tasks:
  check:
    cmds:
      - printf '%s\n' "$TASKFILE_EMBEDDED"
EOF

cat > "$root/justfile" <<'EOF'
check:
  printf '%s\n' "$JUSTFILE_EMBEDDED"
EOF

cat > "$root/check.spec" <<'EOF'
Name: check
Version: 1
Release: 1
%check
printf '%s\n' "$RPM_EMBEDDED"
%files
EOF

cat > "$root/.vscode/tasks.json" <<'EOF'
{"version":"2.0","tasks":[{"type":"shell","command":"printf '%s\\n' \"$VSCODE_EMBEDDED\""}]}
EOF

cat > "$root/.devcontainer/devcontainer.json" <<'EOF'
{"postCreateCommand":"printf '%s\\n' \"$DEV_CONTAINER_EMBEDDED\""}
EOF

cat > "$root/Jenkinsfile" <<'EOF'
printf '%s\n' "$JENKINS_HOST"
EOF

cat > "$root/check.tf" <<'EOF'
printf '%s\n' "$TERRAFORM_HOST"
EOF

cat > "$root/check.nix" <<'EOF'
printf '%s\n' "$NIX_HOST"
EOF

count_variable()
{
  name=$1
  file=$2
  output=$("$BIN" --lint --no-traces "$file" 2>&1)
  printf '%s' "$output" | grep -c "The variable '$name'"
}

github=$(count_variable GITHUB_EMBEDDED "$root/.github/workflows/check.yml")
github_output=$("$BIN" --lint --no-traces \
  "$root/.github/workflows/check.yml" 2>&1)
github_host=$(printf '%s' "$github_output" | \
  grep -Ec 'POWERSHELL_HOST|target-only-ci-command|positional parameter|Expected a command before the pipe')
markdown=$(count_variable MARKDOWN_EMBEDDED "$root/README.md")
container=$(count_variable CONTAINER_EMBEDDED "$root/Containerfile")
makefile=$(count_variable MAKE_EMBEDDED "$root/Makefile")
package=$(count_variable PACKAGE_EMBEDDED "$root/package.json")
gitea=$(count_variable GITEA_EMBEDDED "$root/.gitea/workflows/check.yml")
forgejo=$(count_variable FORGEJO_EMBEDDED "$root/.forgejo/workflows/check.yml")
gitlab=$(count_variable GITLAB_EMBEDDED "$root/.gitlab-ci.yml")
ansible=$(count_variable ANSIBLE_EMBEDDED "$root/playbook.yml")
compose=$(count_variable COMPOSE_EMBEDDED "$root/compose.yml")
cloud_build=$(count_variable CLOUD_BUILD_EMBEDDED "$root/cloudbuild.yaml")
circle=$(count_variable CIRCLE_EMBEDDED "$root/.circleci/config.yml")
azure=$(count_variable AZURE_EMBEDDED "$root/azure-pipelines.yml")
bitbucket=$(count_variable BITBUCKET_EMBEDDED "$root/bitbucket-pipelines.yml")
buildkite=$(count_variable BUILDKITE_EMBEDDED "$root/.buildkite/pipeline.yml")
travis=$(count_variable TRAVIS_EMBEDDED "$root/.travis.yml")
kubernetes=$(count_variable KUBERNETES_EMBEDDED "$root/pod.yml")
drone=$(count_variable DRONE_EMBEDDED "$root/.drone.yml")
woodpecker=$(count_variable WOODPECKER_EMBEDDED "$root/.woodpecker/check.yml")
taskfile=$(count_variable TASKFILE_EMBEDDED "$root/Taskfile.yml")
justfile=$(count_variable JUSTFILE_EMBEDDED "$root/justfile")
rpm=$(count_variable RPM_EMBEDDED "$root/check.spec")
vscode=$(count_variable VSCODE_EMBEDDED "$root/.vscode/tasks.json")
dev_container=$(count_variable DEV_CONTAINER_EMBEDDED "$root/.devcontainer/devcontainer.json")
jenkins=$(count_variable JENKINS_HOST "$root/Jenkinsfile")
terraform=$(count_variable TERRAFORM_HOST "$root/check.tf")
nix=$(count_variable NIX_HOST "$root/check.nix")

printf 'github=%s markdown=%s container=%s make=%s package=%s\n' \
  "$github" "$markdown" "$container" "$makefile" "$package"
printf 'github-host=%s\n' "$github_host"
printf 'gitea=%s forgejo=%s gitlab=%s ansible=%s compose=%s cloud-build=%s\n' \
  "$gitea" "$forgejo" "$gitlab" "$ansible" "$compose" "$cloud_build"
printf 'circle=%s azure=%s bitbucket=%s buildkite=%s travis=%s kubernetes=%s\n' \
  "$circle" "$azure" "$bitbucket" "$buildkite" "$travis" "$kubernetes"
printf 'drone=%s woodpecker=%s taskfile=%s justfile=%s rpm=%s\n' \
  "$drone" "$woodpecker" "$taskfile" "$justfile" "$rpm"
printf 'vscode=%s dev-container=%s jenkins=%s terraform=%s nix=%s\n' \
  "$vscode" "$dev_container" "$jenkins" "$terraform" "$nix"
