# koshka-oriented shell

[![Koshka is at least 5 times faster than Bash](https://github.com/toiletbril/kosh/actions/workflows/ci.yml/badge.svg)](https://github.com/toiletbril/kosh/actions/workflows/ci.yml)

0.1.0 has been released! See the [Release Blog Post](https://fennec.support/scribbles/shell-release).

**The project was renamed in 0.1.1.** The `shit` is now `kosh`, `shitbox` is
`koshkit`, and every prefix is changed to `kosh`.

---

**Koshka** is the Russian word for a cat. The binary is `kosh`, which reads as
Koshka-Oriented SHell.

Have you ever seen warnings from software that uses Bison as its parsing
engine? Did you encounter any of the coreutils' error messages? Have you spent a
day debugging a Bash script? Aren't you tired?

I invite you to notice the interactive prompt, the speed, and the diagnostics:

| Koshka analyzing 20K-line shell script in ~0.05s |
| :-: | 
| ![](assets/demo.gif) |

## What

**Koshka** is an interpreter, interactive shell and diagnostic tool, with
first tier support for Windows, Linux and macOS, fully compatible with Bash 5.3
and Dash. It usually runs at least 5 times faster than Bash and is built to
have the best UX possible.

It aims to be a complete, faster and portable Bash-compatible shell replacement
for systems, runners or development machines that can benefit from it's speed
and pedantic diagnostics.

The shell is friendly to coding agents. Strict validation and source-located
error messages provide clear feedback while shell commands are generated or
repaired.

The shell guarantees first tier support for Linux, macOS and Windows,
preserving equivalent behavior on either of these systems.

The shell is designed to work without absolutely no config whatsoever.
**Koshka**'s Linux binary is static and it does not use C++'s STL. **Koshka**
also can work without coreutils, using it's own utilities.

**The project is in early stage**. There's no guarantee that it will not blow
your computer up. Bug reports are greatly appreciated.

## Three shells in a trenchcoat

[See the manual page](docs/kosh.1) for a fuller explanation:
```bash
$ man docs/kosh.1
```

**Koshka** runs in four modes, called moods, across three shell identities. ZSH
has similar idea behind it's `emulate` builtin.

The default `kosh` mood is a strict superset of Bash with analysis and
optimization enabled. The other moods are `bash`, `bash-posix`, and `sh`. The
`bash-posix` mood provides Bash behavior with its POSIX mode enabled.

Before it runs a command, **Koshka** analyzes and optimizes the complete script.
The catalog includes about 100 checks derived from ShellCheck and about a dozen
native analysis checks.

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

The `KOSH_FLAGS` environment variable specifies default flags. A flag on the
command line still wins.

When `KOSH_FLAGS` or the command line contains an invalid flag or argument, a
login shell skips its startup files and opens a rescue session.

## Additional furballs

The interactive mode is inspired by
[fish](https://github.com/fish-shell/fish-shell). Shell provides syntax
highlighting, word movement, editing controls, UTF-8 support, display-width
handling for wide characters, multiline editing, history search, and persistent
history. Shell does not depend on readline.

**Koshka** has more than 50 builtins, and each builtin supports `--help`. These
include Bash and POSIX builtins. The additional builtins include the following
commands.

- `z` is a port of [zoxide](https://github.com/ajeetdsouza/zoxide).
- `bench` provides built-in benchmark infrastructure inspired by Performance
  Optimizer Observation Platform ([poop](https://github.com/andrewrk/poop)).
- `assimilate` provides transactional installation on an SSH target.

The `koshkit` builtin bundles a BusyBox-style set of small core utilities.

- File utilities include `cp`, `mv`, `ln`, and `rm`.
- Search utilities include `find` and `grep`.
- Process utilities include `killall`, `pkill`, `ps`, `timeout`, and `nproc`.
- Minimal implementations of `calc` and `make` are included.

`koshkit cat --syntax-highlighting` colors shell files when standard output is
a terminal. Shell extensions and known shell shebangs select the source. The
output omits underline attributes.

# Development

This software began as a late April Fools' joke. It is written from scratch in a
macro-heavy C++23 dialect and is compiled with `-nostdlib++`. The executable
links only to the C library.

Development occurs on `staging`, and the branch may be broken. The `master`
branch should pass all tests.

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
targets and cross-compiles release binaries to Linux. MinGW-w64 targets Windows,
osxcross with a macOS SDK targets Darwin arm64, and `cosmoc++` builds the
Cosmopolitan modes.

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

The `$CXXFLAGS` environment variable appends flags to the build commands.

Build with GNU Make as shown below. Make uses every available logical CPU and
shares its bounded job pool with recursive builds.

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

The running binary can install itself on an SSH target with `assimilate
user@host`.

## Roadmap

Is it usable?

- [x] Programs run.
- [x] Linux and Windows are supported.
- [x] Logical sequences are supported with `&&`, `||`, and `;`.
- [x] Pipes are supported.
- [x] Redirections are supported with `>` and `<`.
- [x] Shell expansions are supported with `?`, `[...]`, `*`, and `~`.
- [x] Escapes are supported.
- [x] Environment variables are supported.
- [x] Numeric expressions are supported.

Is it good?

- [x] Background jobs are supported.
- [x] Scripting constructs include flow-control keywords.
- [x] Blocks and functions are supported.
- [x] The shell supports `sh` scripts.

Is it exceptional?

- [x] The shell supports Bash scripts.
- [x] ShellCheck-style warnings are built in.
- [x] Koshkit replaces common Unix programs that are absent from Windows.
- [ ] Arbitrary-precision numeric expressions are planned.
