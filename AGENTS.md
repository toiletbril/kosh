# Koshka project notes

## Documentation ownership

Never modify README.md without explicit approval.

`docs/kosh.1` owns invocation, options, moods, shell syntax, runtime behavior,
builtins, interactive behavior, diagnostics, environment variables, startup
processing, and runtime files. `docs/kosh.5` owns startup file identity and
file format behavior. A new flag, mood, builtin, or renamed option updates
`docs/kosh.1` and `completions/kosh.bash`. A configuration change also updates
`docs/kosh.5`. An architecture or contributor workflow change updates this
file.

The project is a C++ and C command shell. Speed is the defining goal. The
interactive editor is vendored under `src/toiletline`.

## Build and test

The top-level Makefile delegates to `src/Makefile` and supplies the configured
logical processor count. Prefer a make target to a direct compiler command.

`make MODE=rel` builds `./kosh`. `make MODE=dbg` builds `./kosh-dbg` with
AddressSanitizer and UndefinedBehaviorSanitizer. `make MODE=cov` builds
`./kosh-cov`. The default mode is debug. `make clean` owns artifact removal.
Never remove `./kosh` directly.

Run `make test` for the main and completion suites. Run `make bench` for the
benchmark. The completion suite requires the debug binary. Wrap an interactive
launch in a timeout.

The `refill` target regenerates goldens. `REFILL` limits regeneration to named
tests. Read every regenerated golden before accepting it. Every golden lives
directly under `test/expected` and has a unique test name.

Make discovers test inputs and platform skips. Each runner owns its setup,
output, comparison, refill behavior, and cleanup. Runner output is stored under
`.test-work/results`. Auxiliary test shell scripts use two-space indentation.

The native and CLI runners accept test names. A bare `NAME` or `cli_NAME` target
runs one test through its normal runner. The native runner suppresses noisy
diagnostics outside the canonical `shellcheck_static_*` tests.

Every koshkit rm test uses `--dry-run`. Temporary cleanup uses the system rm
after a nonempty path check. The bashdiff and mimicrydiff comparisons require
Bash 5.3 or newer. Set `BASHP` to a modern Bash on macOS.

## Code conventions

Use `let` and `let const` for deduced locals. A literal counter keeps an explicit
integer type. Functions use `fn name(...) throws -> ret`.

Compare pointers with `nullptr`. Do not use pointer truthiness. A boolean name
begins with `is_`, `should_`, `was_`, `did_`, or `has_`. A count ends in
`_count`. A measured number ends in a suffix such as `_length`, `_depth`, or
`_position`. A variable-bound lambda begins with `do_`. An accessor begins with
`get_` or `set_`.

Free structs and enums use lower snake case. Classes and nested types use camel
case. File operations accept `Path`.

A clear name replaces an explanatory comment. C and C++ comments use
`/* ... */`. An if condition containing `&&` or `||` uses braces. Blank lines
separate logical blocks and surround loops and returns.

Three or more name comparisons use a static table. A hot leading-byte dispatch
uses a switch. A static name table uses `consteval StaticStringMap` or
`StaticStringSet` with SSK keys. Their constructors derive the leading-byte and
length prefilter from the table.

Search for an existing function, parser, or helper before adding logic. New
abstractions, file splits, file merges, and dependency upgrades require
approval. Per-executor state passes through `EvalContext` and constructors.

## Front end and evaluation

`src/Main.cpp` drives flags, startup processing, scripts, and the interactive
loop. `src/Lexer.cpp` creates tokens. `src/Parser.cpp` creates the syntax tree.
`src/Optimizer.cpp` folds constants and removes dead branches. Evaluation is
split across `src/Eval.cpp` and the `Eval` prefixed sources. Expressions are
split by command family across the `Expressions` sources. Shared helpers are
declared in `src/ExpressionsInternal.hpp`.

Owned shell source normalizes CRLF before lexing, analysis, evaluation, and
diagnostics. A lone carriage return remains data.

Analysis-only runs stream one top-level and-or chain at a time in two passes.
The first pass gathers suppressions, scopes, directive spans, and parse errors.
The second pass sends each unit through `AnalysisUnitStream` and `analyze_ast`,
then releases its arena span. `top_level_sibling_carry` preserves the owned data
needed by checks across unit boundaries. `FUNCTION_ARENA` stays null during
streaming analysis.

Runtime state owns the mood, diagnostic controls, strictness marks, and shell
options. An explicit nounset, pipefail, or failglob setting survives a mood
change. An explicit `set --mood` clears the diagnostic level selected by `-W`,
`-WW`, or `-WWW`.

Eval snapshots retain shell and shopt state, the directory stack, the working
directory, and the file creation mask. Restricted behavior reads one shared
context state. Forked evaluators report their current process through BASHPID,
while `$$` retains the original shell process.

An asynchronous pipeline job owns every stage process. POSIX stages share one
process group. The final stage remains primary for status and job output. Job
operations retain and reap every stage. Standard stream writes retry partial
writes and reject a zero-length write while bytes remain.

## Platform boundary

`src/Platform.cpp` selects the implementation. POSIX and Windows behavior lives
in their matching platform sources. Platform headers, calls, types, and macros
stay behind `src/Platform.hpp` and the `os` wrappers.

Every wrapper that rebinds a standard descriptor increments the descriptor
epoch. Cached color decisions are refreshed against that epoch. Fork-backed
launches, process groups, filesystem operations, and processor counts also pass
through platform wrappers.

## Completion and language server

`src/Completion.cpp` drives completion. The `Completion` prefixed sources own
manpage, scan, highlight, syntax, path, and cache work. Completion,
highlighting, diagnostics, and koshkit cat share the tolerant scanner and
semantic roles. Completion, highlighting, and command lookup share directory
indexes.

Command completion reads keywords, builtins, bundled utilities, functions,
aliases, and the active PATH. `KEYWORD_ENTRIES` is the single keyword source.
The language server wraps completion in `begin_explicit_completion` and resolves
command documentation lazily.

A document mood is selected from a recognized shebang, then the client language
identifier, then the extension. The `shellscript` identifier selects bash.

`analyze_ast` can collect `analysis_symbol_records`. The language server uses
them for hover, outline, definition, and rename. Ordinary analysis leaves the
out-param null. Assignment records include ordinary, builtin, loop, arithmetic,
read, mapfile, getopts, printf, and declaration binders. Function records retain
body spans.

Rename uses semantic highlight spans from the open document. Variable edits
preserve sigils and braces. Command edits require a function or alias definition
in the document. Variable names use `word_is_plain_identifier`. Command names
use `word_is_function_name`.

`src/ShellVariables.hpp` owns the dynamic variable catalog. Variable completion
and hover respect the active mood. Bare koshkit utility completion is available
in the default mood and when the koshkit option is enabled.

## Diagnostics and locations

`DiagnosticsCatalog.cpp` owns each analysis diagnostic. `DiagnosticsDispatch.cpp`
owns command dispatch. The other `Diagnostics` sources own grouped checks.
`SimpleCommand::analyze` gathers one `command_lint_input`, and whole-script
findings run once at the end of `analyze_ast`.

ShellCheck disable comments apply to the complete file when leading, or to the
next complete and-or command otherwise. A numeric code suppresses every variant
under that code. An exact slug suppresses one variant. Parser and runtime errors
are unaffected.

A `SourceLocation` contains a 32-bit position, length, and source name index.
Source names are interned for the life of the run. Syntax nodes keep their end
position separately. Diagnostics and LINENO share one cached line index.

## Values and allocation

Small types live in lightweight headers. Shared value behavior belongs on the
value type. `ArrayList::find` returns `Maybe<usize>`. Membership checks use
`find().has_value()`.

An `Allocator` is one tagged word for the pooled heap, a bump arena, or fake
storage. Allocation dispatches on its kind. Only heap storage is freed. The
ownership query identifies storage held by a specific bump arena.

`ArrayList` allocates on first growth and uses 32-bit length and capacity.
`String` has inline storage and uses an exact first heap allocation. `SparseList`
is used for member lists that are almost always empty.

A bump arena registers destructors for nontrivial objects unless the type
declares `is_arena_destructor_noop`. The marker is valid only when every
reachable resource has arena lifetime or needs no cleanup. Destructor records
use fixed 64 KiB chunks.

`WordSegment` is 32 bytes on 64-bit targets. Its source span, kind, and flags are
packed into two 32-bit units. Parsed text, cache metadata, flattened values, and
segment lists move into the token arena before token construction. Runtime
copies retain heap ownership. Parsed word token destructors therefore need no
registration.

A segment evaluation cache holds substitution, arithmetic, folded result, and
lifetime identities. Parsed caches use the syntax or function arena. Runtime
copies use the heap. Lifetime checks reject stale cache contents after arena
reuse.

## Finishing a change

Project workflow mistakes are recorded in [MISTAKES.md](MISTAKES.md). Every
mistake recorded there must have a prevention rule here.

Source changes use the approved patch or edit tool. Shell text commands inspect
files and output only. Offer parallel read-only agents before repository
research. Read the matching guidance file before an implementation or prose
edit. Confirm that an edit changes its target before calling the edit tool.
Avoid repeating a read after the harness reports that the file is unchanged.
Reuse current guidance file results while they remain available in the conversation.
Compare the complete source and replacement text before calling the edit tool.
Search for the smallest unique exact segment before editing a long golden.
Validate a shell probe encoder with a small payload before sending a large
source file. Interactive probes use an available bounded runner. Complete a
type migration across every use before compiling it. Print the required change
table after each edit batch. Use the dedicated edit tool when the shell has no
approved patch command. Background agent results and progress arrive through
completion notifications and `SendMessage`. Never pass agent identifiers to
`TaskOutput`, including while an agent is running. A continuation must reread
the matching guidance before its first edit and offer parallel read-only agents
before restarting repository research. Do not retry an invalid task lookup
after the notification has reported the result. Broad searches must use
existing paths or directory globs. Do not inspect or edit code
covered by an active read only agent. Do not inspect or edit tests covered by an
active read only agent. Teammate replies require a concrete
reachable agent name or identifier. Agent launches use a type listed by the
current session. Performance comparisons use the release binary explicitly.
Locate a focused test target before invoking it from the repository root.
Identify the input type before selecting its focused test target.
Use a bounded temporary path and verify cleanup before a workload probe.
Check active agent ownership before a test edit. Never poll a running agent
through `TaskOutput`. Read the exact local segment before editing an unverified
file area. Independent probes run as separate commands after their paths are
resolved. Read only shell probes use the simplest command form that preserves
the input. Command options precede the path separator. Read only reviews stream
inspection output without creating files. Large patches use a raw template
string or separate small edit calls. Patch wrappers avoid delimiter characters
contained in the patch text.
Compile migrations must check every constructor input and conditional result
type before the build. Mixed file patches must use separately verified exact
anchors.
Every borrowed view must retain its owning local for the complete use span.
Multi-file patches must be split when a deletion-only hunk ends a file change.
Static proof helpers must match the exact evidence level required by the
behavior.
Control-flow changes must verify operator ownership in the printed AST. Every
failed mixed patch must be followed only by single-file patches.
Transcript probes must limit both matched records and output bytes before
reading content. Active review ownership must be resolved before any
overlapping inspection.
Every SourceLocation subspan must prove both offset and length bounds.
Control-flow merge probes must use a nonconstant condition when they need
branch uncertainty. Exact patch anchors must be copied from the latest
inspection.
Language server probes must complete initialize before any document request.
Shell search patterns containing command syntax must use single quotes.
Catalog availability checks must use established behavior predicates.
Fallback executable probes must print the selected path.
Guidance probes must resolve available paths before reading several files.
Content-driven build invalidation must have an explicit dependency when equal
timestamps are possible. Platform timing probes must verify the host tool
interface before a long run. Build verification must compare embedded version
metadata with the current commit. Protocol workload probes must redirect the
complete emitter group. Asynchronous workload wrappers must capture and print
the status after `wait`. Cwd-sensitive test scripts must run through their
owning harness or from their declared working directory. Serial submake probes
must clear inherited jobserver flags before they set their own job count.
Sampling reports must use a validated regular output file and capture the target
status separately. Long workload probes must use an operating-system timeout
because tool yielding does not stop them. Profiling harnesses must use the exact
resolved interpreter path. Protocol profilers must syntax and scope check every
reporting path on a small payload before the full input. A macOS workload binary
must be force signed after every release relink.
Container migrations must verify which overload each const member actually
selects before compiling.
Profiler report cleanup must validate a nonempty exact path and use the system
removal command without force.
Commit subject length must be checked before creating the commit.
Golden paths must be resolved from the owning test name before inspection.
Rejected temporary cleanup must move the validated exact path recoverably to
Trash.
An optional tool path must be checked with an explicit branch before
invocation.
Regression fixtures must keep diagnostic control names distinct from the names
under test.
Logging edits must count every format placeholder and argument before the patch
is applied.
Adapter tasks must trace the current shared call path before changing the shared
owner.
Debug-only driver probes must verify the debug artifact before invocation.

Format the changed files with the owning project tool. Run focused tests after a
meaningful batch. Read regenerated goldens. Run `git diff --check` and inspect
the complete diff. Confirm that README.md remains untouched without explicit
approval. Commit completed work and never push or create a pull request or issue
without an explicit request.
