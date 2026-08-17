# Koshka project notes

## Documentation ownership

Never modify README.md without explicit approval.

The runtime manual is docs/kosh.1. It owns invocation, options, moods, shell
syntax, runtime behavior, builtins, interactive behavior, diagnostics,
environment variables, startup processing, and runtime files.

The configuration manual is docs/kosh.5. It owns startup file identity and
file-format behavior. Startup files contain ordinary shell commands. Shell
language and runtime loading rules remain in docs/kosh.1.

A new flag, mood, builtin, or renamed option updates docs/kosh.1 and
completions/kosh.bash. A configuration file change also updates docs/kosh.5.
An architecture or contributor workflow change updates AGENTS.md.

The project is a C++ and C command shell. Speed is the defining goal. The
interactive editor is the vendored C submodule under src/toiletline.

## Build and install

The top-level Makefile delegates to src/Makefile. It supplies the configured
logical processor count when the caller does not select a parallel job count.

`make MODE=rel` writes the optimized binary to ./kosh. `make MODE=dbg` writes
./kosh-dbg with AddressSanitizer and UndefinedBehaviorSanitizer. `make MODE=cov`
writes ./kosh-cov. The default mode is dbg.

A bare `make` builds the `kosh` target from a clean checkout. The `kosh` target
is the default goal. Object directories are order-only prerequisites. Prefer a
make target to a raw compiler invocation.
Use `make clean` to remove stale artifacts. Never remove ./kosh directly. The
clean target removes the main binaries, object trees, and every Cosmopolitan
.dbg and architecture ELF sidecar.

The completion suite requires the debug binary because
`--debug-complete-at` is unavailable under NDEBUG. The README documents the
complete mode catalog, cross-compilation targets, and PREFIX installation.

## Test and golden workflow

Run `make test` for the main and completion suites. Run `make bench` for the
benchmark. Wrap an interactive launch in a timeout.
The debug test step reports its elapsed time. It warns after 180 seconds and
fails after 300 seconds.

The `refill` target regenerates goldens. The REFILL variable limits regeneration
to named tests. Read every regenerated golden before accepting it. Refill
records the binary output without judging it.

Every golden lives directly below `test/expected`. The directory has no
subdirectories. Golden-backed test names remain unique across every harness.

Make discovers test inputs and keeps platform skip lists. Each Make recipe
launches one runner under `test`. A runner never invokes Make. The suite runner
starts the harness runners and bounds their parallel workers. Each harness
runner owns its process setup, output capture, comparison, refill behavior, and
cleanup. Auxiliary test shell scripts use two-space indentation.

The native, CLI, build, completion, highlight, interactive, compatibility, and
benchmark harnesses each have one runner. The native and CLI runners accept
test names. A bare NAME target or cli_NAME target runs one test through the
same runner. The compatibility runner compares explicit moods and mimicry
against one shared reference result. The dashdiff, bashdiff, and mimicrydiff
targets select that runner. The harness carries alternate goldens for
documented macOS differences.

The native runner suppresses annoying diagnostics outside the
`shellcheck_static_*` family. Those canonical tests own the diagnostic catalog.
A test for another parser or evaluator contract uses strict input and does not
duplicate diagnostic output.

Runner output files live below `.test-work/results`.

The speed gate compares the slowest reference time with the fastest candidate
time.

Every rm test invokes the koshkit rm with `--dry-run`. This rule applies to
koshkit_rm and every new rm test. Temporary directory cleanup uses the system rm
behind a `[ -n "$d" ]` guard. The koshkit rm under test never performs cleanup.

The bashdiff and mimicrydiff comparisons require Bash 5.3 or newer. Both scripts
report a skipped comparison when BASHP names an older Bash. The macOS system
/bin/bash is Bash 3.2. Pass a modern Bash through BASHP on macOS.

## Code conventions

### Declarations and names

Use `let` and `let const`, the macros for `auto` and `const auto`, for deduced
locals. A literal counter such as `usize i = 0` keeps its explicit type because
`let i = 0` would deduce int. Functions use `fn name(...) throws -> ret`.

A null pointer comparison uses `== nullptr` or `!= nullptr`. Never use pointer
truthiness.

Names are verbose and semantic. A boolean begins with `is_`, `should_`, `was_`,
`did_`, or `has_`. A count ends in `_count`. A measured number ends in a suffix
such as `_length`, `_depth`, or `_position`. Never use a bare `n_` prefix. A
variable-bound lambda begins with `do_`. An accessor begins with `get_` or
`set_`.

Stray enums and structs use lower_snake_case. Classes, nested enums, and nested
types use CamelCase. File operations accept Path, never String or StringView.

### Comments and control flow

A clear name replaces a comment that explains an unclear name. A comment states
why the code has its current shape. C and C++ comments use `/* ... */`. Never
use `//`.

An if condition containing `&&` or `||` uses braces. A trivial single-condition
if omits braces. Blank lines separate logical blocks. Place a blank line before
and after a loop, before a return, and after a declaration group.

Three or more name comparisons use a static table. A hot leading-byte dispatch
uses a switch. A static dispatch table uses `consteval StaticStringMap` and SSK
keys.

### State and reuse

Per-executor state passes through EvalContext and constructors. The codebase has
no mutable global for per-executor state.

Search for an existing function, parser, or helper before implementing new
logic. Reuse the existing mechanism. New abstractions, file splits, file merges,
and dependency upgrades require approval.

## Architecture

### Front end and evaluation

src/Main.cpp parses flags, runs the startup chain, and drives scripts or the
interactive loop. src/Lexer.cpp creates tokens. src/Parser.cpp creates the
syntax tree. src/Optimizer.cpp folds constants and removes dead branches during
analysis.

A C-style for loop whose condition folds to zero is removed only when its init
clause is empty. A nonempty init clause runs once before the condition.

Owned shell source normalizes CRLF pairs before lexing, analysis, evaluation,
and diagnostics. Named files, standard input, command strings, sourced text,
and executable fallback use the same normalization. A lone carriage return
remains data.

Evaluation is divided among src/Eval.cpp and the Eval-prefixed files. These
files own substitution, word expansion, parameter expansion, globbing,
arithmetic, arrays, source, jobs, and functions. src/Expressions.cpp owns the
command node base and analysis hooks. src/ExpressionsSimpleCommand.cpp owns
simple commands. src/ExpressionsCompound.cpp owns lists, pipelines, loops,
case, and compound commands. src/ExpressionsArith.cpp owns arithmetic and
logical nodes. Shared free helpers are declared in
src/ExpressionsInternal.hpp.

The builtins live under src/builtins. The bundled utilities live under
src/koshkit. Every builtin remains enabled. The enable `-d`, `-n`, `-f`, and
`-s` flags are accepted without effect. The `-a` flag lists every builtin.

### Runtime state

MimicMood.hpp owns `parse_mood_name` and `mood_name`. The flag parser,
`set --mood`, and `set --init-moods` use that table.
`detect_mimic_shell_from_extension` owns the shell extension table, and
`Path::is_shell_source` and the language server both read it.

RuntimeState owns the mood, diagnostic controls, explicit strictness marks, and
shell option bits. Capture and restore copy the complete state. The set builtin
uses one descriptor table for mutation, queries, help, completion, SHELLOPTS,
and `$-`. Its compile-time name map retains binary search.

Builtin and utility flag metadata uses immutable string views and fixed
registries. Parsing state is reset for each invocation. The shared parser uses
local argument pointer storage for ordinary command sizes and preserves Bash
syntax statuses and repeated option values.

`apply_strictness_for_mood` owns mood strictness. An explicit nounset, pipefail,
or failglob setting survives a mood change. `command_word_is_glob` owns the
command-position glob check. The runtime diagnostic levels distinguish strict,
lenient, and annoying diagnostics. The `-W`, `-WW`, and `-WWW` forms select the
level. An explicit `set --mood` clears that selection. The
`annoying-diagnostics` set option controls the annoying tier, and
`no-diagnostics` skips analysis. The `--no-annoying-diagnostics` flag disables
the annoying tier at startup. The lint flag overrides both controls.

Restricted behavior uses one shared context state. Variable changes, directory
changes, slash-bearing command and source operands, output redirections, exec,
command `-p`, enable loading, history paths, and hash `-p` read that state.

Eval snapshots also retain shopt state, the directory stack, the working
directory reference, and the file creation mask. An in-process subshell restores
the complete snapshot.

Sparse indexed array names are tracked separately. Resetting a dense array does
not scan unrelated sparse entries. A one-element PIPESTATUS update reuses its
dense slot.

### Jobs and process execution

An asynchronous pipeline job owns every stage process. POSIX stages share one
process group. The final stage remains primary for `$!`, status, and job output.
Polling, waiting, foregrounding, backgrounding, signaling, and disowning retain
or reap earlier stages. A stopped event from any retained stage remains stored.
The wait builtin returns the first stored stopped status without waiting for
that process again. The fg and bg builtins resume every retained stage.

A process-group reference retained for timeout remains valid after polling
closes the leader. `close_process_group` releases the retained platform handle.
An interactive timeout child waits behind a start pipe until its process group
owns the controlling terminal.

Standard output and standard error writes retry partial platform writes. A zero
length write is treated as a failure while bytes remain.

Forked evaluators report the current process through BASHPID. `$$` retains the
original shell process.

Executable-format fallback uses an explicit invalid-process result. A fresh
evaluator receives the command environment and argument zero. Caller variables,
functions, and traps are excluded. Each complete top-level command runs before
the next command is parsed. Terminal execution begins only after the lexer
reaches the literal end of source.

The main shell ignores SIGPIPE. A forked child restores the default action.

### Platform boundary

src/Platform.cpp routes the operating system implementation. POSIX targets use
PlatformPosix.cpp as the base, PlatformPosixFilesystem.cpp for filesystem
operations, PlatformPosixProcess.cpp for process operations, and
PlatformPosixExtra.cpp for Linux and Darwin overlays. Windows uses the matching
PlatformWin32 base, filesystem, and process files. Cosmopolitan flags are
registered by the POSIX implementation.

Platform.hpp owns platform headers. Every platform call and platform type is
hidden behind an os wrapper. A non-platform source contains no syscall, platform
header, or platform macro.

`read_fd` reports a closed pipe as EOF on every implementation. The shared
`get_processor_counts` wrapper supplies the affinity-limited and configured
logical processor counts used by koshkit nproc.

Every wrapper that rebinds a standard descriptor bumps the counter reported by
`get_descriptor_epoch`. The terminal answers behind `stdout_wants_color` and
`stderr_wants_color` are cached against that counter, and a rebinding refreshes
them. Diagnostic rendering asks once per message and pays no isatty call.

Fork-backed evaluator launches pass through os wrappers. POSIX evaluates the
inherited syntax tree in the child. Windows selects an in-process fallback or
starts a fresh shell from recorded source. When Windows cannot fork a piped
evaluator, a context-independent builtin or valid koshkit utility starts as a
fresh shell stage. Platform flags and runtime initialization also pass through
os wrappers.

On Windows, a background process receives a fresh console process group without
a Job Object. Successful virtual-terminal initialization enables editor
decorations without requiring TERM.

### Completion and editor

src/Completion.cpp drives completion. src/CompletionManpage.cpp and
src/CompletionScan.cpp own their scans. src/CompletionHighlight.cpp owns the
per-keystroke highlighter. src/CompletionInternal.hpp declares shared helpers.
src/CompletionPolicy.hpp owns program policies, help allowlists, extension
hints, custom completer routing, and transparent prefixes.

Completion, highlighting, diagnostics, and koshkit cat share the tolerant
scanner and semantic highlight roles. Completion, highlighting, and command
lookup share directory scans. PATH changes invalidate the derived indexes and
the execution hash.

DiagnosticsCatalog.cpp owns each analysis diagnostic's code, slug, summary,
message, suggestion, related detail, tier, and delivery.
DiagnosticsDispatch.cpp owns the command name dispatch table. The remaining
Diagnostics-prefixed sources own the grouped check bodies and their shared
internal helpers. SimpleCommand::analyze gathers the borrowed arguments,
redirections, prefix assignments, command literal, and dispatch record into one
command_lint_input and hands that bundle to the check entry points, so a new
check adds no traversal. Expressions.cpp keeps the walk and the reporting
funnel. Expression analysis reports a diagnostic ID with source locations and
dynamic values. The default mood rejects strict and lenient findings and
reports annoying findings as warnings.
Compatibility moods expose the tiers as warnings through `-W`, `-WW`, and
`-WWW`. A `static_assert` couples catalog order to the `diagnostic_id` enum.
A message authors shell syntax in backquotes. A dynamic name, path, value, or
number is single quoted. A summary begins with a lowercase letter, and a
suggestion begins with an uppercase letter.

The portability rows are gated behind `shebang_is_posix_sh`. Whole-script
dataflow findings are reported by one sweep at the end of `analyze_ast`. Each
analyzed root follows every readable static source path once and skips dynamic
or unavailable source operands.

`analyze_ast` accepts an optional `analysis_symbol_records` out-param that
collects each variable assignment and each function body span. The language
server passes one and every other caller leaves it null, so an ordinary run
records nothing. A hover answer is built from those records and the open
document source, because the server returns before the startup chain runs and
holds no user variables or functions.

A document mood is selected from a recognized shebang, then from the client
language identifier, then from a recognized document extension. The
`shellscript` identifier selects the bash mood.

Variable completion includes the dynamic variables available in the active
mood. Builtin command completion includes every builtin. Bare koshkit utility
completion is active in the default mood and when the koshkit option is enabled.

The analysis stage accepts ShellCheck `disable` comments. A leading directive
applies to the complete file. A later directive applies to the next complete
and-or command. A numeric code suppresses every catalog variant under that
code. An exact slug suppresses one numbered or native analysis variant. Parser
errors and runtime diagnostics are not suppressed by these comments.

SC1015, SC1016, SC1117, SC1119 through SC1122, SC2034, SC2278, and SC2280 are
excluded. These codes are retired, too noisy, require unavailable syntax data,
require another traversal, or apply only to an unsupported shell mood.

src/Toiletline.cpp connects the editor and evaluator. The vendored editor lives
in src/toiletline/toiletline.h. The completion bridge retains its result until
the editor consumes returned pointers.

The interactive terminal title shows the current user and directory while idle
and the foreground program command line while it runs.

### Diagnostics and source locations

src/Errors.cpp renders located carets and trailing notes. Diagnostic identifiers
live in src/Diagnostics.hpp. A type whose name contains WithLocation owns or
inherits a source location. A type whose name contains WithDetails owns a
trailing note. The semantic classes remain separate for catch routing.
ErrorWithLocationAndDetails may store a second location.

`relocate_error` wraps an unlocated error with a span and retains its details.
Word segment locations survive parameter modifiers, array subscripts,
arithmetic expressions, and nested substitutions. A contiguous here-document
retains its body location. A tab-stripped here-document has no source mapping.

Source traces are attached by eval, command substitution, function
substitution, and process substitution. A frame is printed once while its source
frame remains live. Several `-c` roots retain the source that produced each
message. Diagnostics and LINENO share one cached source line index. Cached
highlight spans use heap storage and survive highlighter arena resets. Display
width and clipping consume borrowed source views.

Directory builtins route every directory change through cd. The directory stack
lives on EvalContext. Logical PWD and OLDPWD are maintained in one place.
Missing paths retain the first unavailable component for the diagnostic span.

### Builtins and utilities

The fc builtin reads decoded events from KOSH_HISTORY. The accepted interactive
event number is retained on EvalContext, so ordinary selection excludes the
active fc command. Listing keeps that event. Execution and editing replace it
with the command that is run. Edited commands run in the current shell after a
named temporary file is removed. The editor is selected from an explicit
option, FCEDIT, EDITOR, and the mood default in that order.

The assimilate transaction copies the running executable through scp. The
remote transaction uses explicit koshkit utilities. The remote login shell must
be POSIX-compatible and able to start the transferred executable.

The candidate SHA-256 identity must match the local executable. A keeper process
holds the transaction lock until the child exits, including after its launcher
exits. A later transaction recovers published and orphaned journals. A handled
failure restores the prior file or symlink and removes transaction files. A
failure before bootstrap cannot alter the installed target. An unusable partial
upload may remain.

KOSH_IDENTITY is a read-only exported dynamic variable. Its lowercase CRC-32
value is computed once on first read or before a child starts. An inherited
value is removed before evaluation begins.

KOSH_GIT_BRANCH, KOSH_GIT_AHEAD, and KOSH_GIT_BEHIND are always-dynamic
variables. KOSH_GIT_BRANCH reads the branch name from .git/HEAD. KOSH_GIT_AHEAD
and KOSH_GIT_BEHIND read the local and upstream SHAs from the filesystem and
fork one git rev-list --left-right --count command only when the SHAs diverge.
Both are empty outside a repository, with no upstream, or when the count is
zero. The branch is read lazily. Ahead and behind share one snapshot for each
evaluated command.

The koshkit cat highlighter selects recognized shell extensions and shebangs.
It is suppressed for null bytes and redirected output. Line numbering remains
continuous across file and standard input boundaries. Highlighting emits no
underline attributes.

Plain cat, grep, tee, uniq, and wc process input through bounded buffers. Cat
retains complete input only when numbering or highlighting requires source
context.

## Value types and allocation

Small types live in lightweight headers. MimicMood.hpp owns mimic_mood.
RuntimeState.hpp owns RuntimeState. NameValueArg.hpp owns NameValueArg and its
`from` factory.

A factored data structure lives directly in the koshka namespace. A factored
class method is defined inline in its header. A free helper whose receiver is a
value type becomes a method on that type. Existing examples include
`StringView::is_all_decimal_digits`, `String::replace`, and
`Path::read_entire_file`.

`ArrayList::find` returns `Maybe<usize>`. Membership checks use
`find().has_value()`. Logic shared by POSIX and Windows lives in Utils.cpp.

ArrayList allocates nothing during default construction and grows
geometrically. String has a small inline buffer and grows geometrically. A
scratch arena uses mark and release lifetime within one scope.

WordSegment retains its source position beside a folded arithmetic result. A
DEBUG trap or xtrace reevaluates the original expression. The 64-bit layout is
168 bytes.

## Logging

The log macros live in src/Trace.hpp. `LOG(level, fmt, ...)` prints at or below
the active verbosity. `LOG_VARS` prints named variables. The levels are Nothing,
Info, Debug, and All. Both macros compile out of a release build.

The executable logging and optimizer flags are documented in docs/kosh.1.

## Finishing a change

Format the changed files with the project tools that own their format. Run the
focused tests that cover the changed behavior. Read every regenerated golden.
Run `git diff --check` and inspect the complete diff.

Confirm that README.md remains untouched unless approval was explicit. Apply the
documentation ownership rules at the start of this file before the change is
finished.
