# koshka-oriented shell

[![Koshka is at least 3 times faster than Bash](https://github.com/toiletbril/kosh/actions/workflows/ci.yml/badge.svg)](https://github.com/toiletbril/kosh/actions/workflows/ci.yml)

0.1.0 has been released! See the [Release Blog Post](https://fennec.support/scribbles/shell-release).

**The project was renamed in 0.2.0.** `shit` became `kosh`, `shitbox` became
`koshkit`, and all prefixes now use `kosh` or `KOSH_`. I got over the funny
name. Thanks.

---

**Koshka** is the Russian word for a cat.

**Koshka** is an absurdly fast and tiny cross-platform interpreter, an
interactive shell, a formatter, and a language server. Koshka is fully
compatible with Bash 5.3 and Dash.

Koshka includes about 300 built-in ShellCheck diagnostics. It is usually five
times faster than Bash and more than 100 times faster than ShellCheck.

| Koshka analyzes a 400,000-line shell script in about 1,5 seconds |
| :-: | 
| ![](assets/lint-demo.gif) |

Besides shell scripts, Koshka is able to analyze common supported formats for
mistakes in Bash code, including the following:

- GitHub Actions, Gitea Actions, Forgejo Actions, and GitLab CI.
- CircleCI, Azure Pipelines, Bitbucket Pipelines, Buildkite, Travis CI, Google
  Cloud Build, Drone, and Woodpecker.
- Ansible, Dockerfile, Containerfile, Compose, and Kubernetes files.
- Makefiles, Justfiles, Taskfiles, and RPM spec files.
- Markdown files, `package.json` scripts, VS Code tasks, and Dev Container
  files.

| Koshka provides instant shell diagnostics for Scripts, Dockerfiles, GitHub CI, Ansible via LSP |
| :-: | 
| ![](assets/lsp-demo.gif) |

## What

`kosh` is the **Koshka** binary.

**Koshka** aims to be a complete, fast, portable replacement for ShellCheck and
Bash. Linux, macOS, and Windows are first-tier support platforms with
equivalent behavior on all three systems.

The shell is designed to work sensibly without any configuration. The Linux
binary is static and does not use the C++ standard library. **Koshka** can use
its own utilities when coreutils are unavailable. 

**The project is at an early stage.** It may still blow up your computer. Bug
reports are welcome.

## Shell linter, formatter, and language server

`kosh --lint` checks complete Bash and POSIX shell syntax and about 300
built-in ShellCheck and native diagnostics. It reads shell source from standard
input, `-c` command strings, or multiple files.

In host files, the linter analyzes only embedded `sh`, `bash`, or `kosh`
regions while rejecting unsupported files.

ShellCheck disable comments accept diagnostic numbers and Koshka diagnostic
names. With files, `--apply` writes non-conflicting safe fixes and reports the
remaining diagnostics.

`kosh --format` formats standard input or one named file without running it. It
uses two-space indentation and wraps at safe token boundaries within 80
columns. Use `--apply` to overwrite files with new formatting.

The formatting style cannot be configured.

`kosh --as-language-server` communicates over standard input and output. It
provides diagnostics, quick fixes, completion, navigation, command help,
semantic tokens, a document outline, and rename support.

The language server recognizes the same embedded shell regions as the linter.
The editor's host language service handles the surrounding syntax.

## Interactive shell and command interpreter

For more details, see the [manual page](docs/kosh.1).

```bash
$ man docs/kosh.1
```

**Koshka** runs win four moods across three shell identities. Zsh provides a
similar feature through its `emulate` builtin.

The default `kosh` mood is a strict superset of Bash with analysis and
optimization enabled. The other moods are `bash`, `bash-posix`, and `sh`. The
`bash-posix` mood provides Bash behavior with its POSIX mode enabled.

Before running a command, **Koshka** analyzes and optimizes the complete script.

The `--mood` option, or `-M`, selects `kosh`, `bash`, `bash-posix`, or `sh`.
The default is `kosh`. A binary symlinked as `sh`, `dash`, or `bash` selects
the matching mood and disables diagnostics. `set --mood` changes the mood at
runtime. In the default mood, `-W` retains the default severities, `-WW`
demotes lenient errors to warnings, and `-WWW` also demotes strict errors. In
other moods, `-W` enables strict warnings, `-WW` also enables lenient warnings,
and `-WWW` also enables annoying warnings.

The `-I` option enables mimicry. **Koshka** detects `sh`, `dash`, and `bash`
shebangs and runs each script in the matching mood. The current diagnostics
setting is preserved.

The `--init-moods` option, or `-L`, accepts a comma-separated list of moods whose
startup files will be used. Its default value is the selected mood.

The `KOSH_FLAGS` environment variable sets default flags. Command-line flags
override them.

When `KOSH_FLAGS` or the command line contains an invalid flag or argument, a
login shell skips its startup files and opens a rescue session.

### Additional furballs

The interactive mode takes inspiration from
[fish](https://github.com/fish-shell/fish-shell). It provides syntax
highlighting, word movement, editing controls, UTF-8 support, display-width
handling for wide characters, multiline editing, history search, and persistent
history. `kosh` does not use readline, so readline configuration is ignored.

**Koshka** has more than 50 builtins, including Bash and POSIX builtins. Every
builtin supports `--help`. Additional builtins include the following commands.

- `z` is a port of [zoxide](https://github.com/ajeetdsouza/zoxide).
- `bench` provides built-in benchmark infrastructure inspired by Performance
  Optimizer Observation Platform ([poop](https://github.com/andrewrk/poop)).
- `assimilate` provides transactional installation on an SSH target.

The `koshkit` builtin bundles a BusyBox-style set of small core utilities.

- File utilities include `cp`, `mv`, `ln`, and `rm`.
- Search utilities include `find` and `grep`.
- Process utilities include `killall`, `pkill`, `ps`, `timeout`, and `nproc`.
- Minimal implementations of `calc` and `make` are included.

**Koshka** also implements arbitrary precision arithmetic, including floats, in
`calc` builtin and in the default mood.

# Development

This software began as a late April Fools' joke. It is written from scratch in a
macro-heavy C++23 dialect and is compiled with `-nostdlib++`. The executable
links only to the C library.

Development happens on `staging`. The branch may be broken. The `master` branch
should pass all tests.

## Prerequisites

A native build needs the following tools.

* Install GNU Make, Clang 18 or later with C++23 support, libc development
  files, and headers for the target platform. Linux builds also need Linux
  kernel headers.
* The default debug build needs the AddressSanitizer and
  UndefinedBehaviorSanitizer runtimes from `compiler-rt`.
* The test suite needs Bash 5.3, Dash, and Python 3.
* The build and test scripts need `mkdir`, `rm`, `cp`, and `printf` from the
  host.
* The full test suite needs `cat`, `cmp`, `diff`, `find`, `grep`, `head`, `sed`,
  and `strings`. Interactive tests also need `script` and `stty`. Process
  supervision needs `setsid` or Perl.

A complete Alpine setup can be installed with the following package set.

```bash
apk add --no-cache \
  git git-doc make build-base musl-dev linux-headers clang llvm lld \
  compiler-rt bash dash zsh yash busybox coreutils mandoc python3
```

The benchmark needs Bash, Dash, and Python 3. Zsh, Yash, and BusyBox ash provide
optional comparison rows. The coverage report needs `llvm-profdata` and
`llvm-cov` from the matching LLVM installation. Documentation checks use
`mandoc`. Formatting and static checks use `clang-format` and `clang-tidy` from
Clang 18 or later.

Each cross-compilation target needs its matching toolchain. Zig builds the Zig
targets and cross-compiles Linux release binaries. MinGW-w64 builds Windows
targets. Osxcross with a macOS SDK builds Darwin arm64 targets. `cosmoc++` builds
the Cosmopolitan modes.

The `MODE` variable controls the build type.

* `rel` is an optimized build.
* `prof` is an optimized build with debug symbols for profiling.
* `cov` is an optimized build with debug symbols for collecting coverage.
* `dbg` includes all symbols, AddressSanitizer, and UndefinedBehaviorSanitizer.
* `cosmo` is an optimized build that uses `cosmoc++` from the Cosmopolitan
  toolchain.
* `cosmo_dbg` is a debug Cosmopolitan build.

`TARGET` defaults to the host platform and accepts `Linux`, `Windows_NT`, or
`Darwin`.
A non-Windows host cross-compiles `TARGET=Windows_NT` with MinGW. A non-Darwin
host cross-compiles `TARGET=Darwin ARCH=arm64` with osxcross. Linux is a native
target.

The `CXXFLAGS` environment variable appends flags to the build commands.

```bash
$ make MODE=<rel/prof/dbg/cov/cosmo/cosmo_dbg>
$ make MODE=rel TARGET=Windows_NT
$ make MODE=rel TARGET=Darwin ARCH=arm64
$ ./kosh --help
```

Zig can also build the `dbg` and `rel` modes.

```bash
$ zig build --release=fast
$ ./zig-out/bin/kosh --help
```

Install or uninstall the selected build with the following commands.

```bash
$ export PREFIX=/usr/local
$ make install
$ make uninstall
```

Assuming the same arch and target, the running binary can install itself on an
SSH target with a builtin command: `assimilate user@host`.
