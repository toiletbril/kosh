# Koshka project rules

## Project

Koshka is a C and C++ shell. Speed is the defining goal. The interactive editor
is vendored under `src/toiletline`.

Never modify README.md without explicit approval.

`docs/kosh.1` owns invocation, options, moods, syntax, runtime behavior,
builtins, diagnostics, environment variables, startup, and runtime files.
`docs/kosh.5` owns startup file identity and format. New or renamed flags,
moods, and builtins update `docs/kosh.1` and `completions/kosh.bash`.
Configuration changes also update `docs/kosh.5`. Architecture and workflow
changes update this file.

## Code

- Use `let` and `let const` for deduced locals. Literal counters keep an integer
  type. Functions use `fn name(...) throws -> ret`.
- Compare pointers with `nullptr`. Do not use pointer truthiness.
- Boolean names start with `is_`, `should_`, `was_`, `did_`, or `has_`. Counts
  end with `_count`. Measurements name their unit. Lambdas start with `do_`.
  Accessors start with `get_` or `set_`.
- Free structs and enums use lower snake case. Classes and nested types use
  camel case. File operations accept `Path`.
- Prefer names to comments. C and C++ comments use `/* ... */`. Brace conditions
  containing `&&` or `||`. Separate logical blocks, loops, and returns with blank
  lines.
- Use a static table for three or more name comparisons and a switch for hot
  leading-byte dispatch. Static name tables use `consteval StaticStringMap` or
  `StaticStringSet` with SSK keys and derived byte and length filters.
- Search for an existing owner, helper, parser, container, and dependency first.
  New abstractions, file splits, file merges, and dependency upgrades require
  approval. Per-executor state passes through `EvalContext` and constructors.

## Architecture

- `src/Main.cpp` owns flags, startup, scripts, and the interactive loop.
  `src/Lexer.cpp` creates tokens. `src/Parser.cpp` creates the syntax tree.
  `src/Optimizer.cpp` folds constants and dead branches.
- Evaluation is split across `src/Eval.cpp` and the `Eval` sources. Expression
  families live in the `Expressions` sources. Shared helpers are declared in
  `src/ExpressionsInternal.hpp`.
- Declarations from an `Internal` source use `koshka::internal` or the owning
  namespace followed by `internal`.
- Owned source normalizes CRLF before lexing, analysis, evaluation, and
  diagnostics. A lone carriage return remains data.
- Analysis streams one top-level and-or chain in two passes. The first gathers
  suppressions, scopes, directive spans, and parse errors. The second uses
  `AnalysisUnitStream` and `analyze_ast`, then releases the arena span.
  `top_level_sibling_carry` keeps cross-unit data. `FUNCTION_ARENA` stays null.
- Runtime state owns moods, diagnostics, strictness marks, and shell options.
  Explicit nounset, pipefail, failglob, and extended-arithmetic states survive
  mood changes. Explicit `set --mood` clears the level from `-W`, `-WW`, or
  `-WWW`.
- Eval snapshots keep shell and shopt state, directories, the working directory,
  and the file creation mask. Restricted behavior uses one context state.
  BASHPID identifies forked evaluators. `$$` identifies the original shell.
- An asynchronous pipeline job owns and reaps every stage. POSIX stages share a
  process group. The last stage owns status and job output. Stream writes retry
  partial writes and reject zero-length writes while bytes remain.
- A named-pipe server connects before its child evaluates source. Thread launch
  order is not connection readiness.
- A parent closes each unused pipe endpoint after CreateProcess so readers can
  observe EOF when the child exits.
- Process-substitution cleanup connects and closes an unused named-pipe path
  before it reaps a child waiting for a client.
- Windows named-pipe redirections use OPEN_EXISTING for every shell open mode.
- Recheck mutable runtime state after any startup file that can change it.

## Platform

- `src/Platform.cpp` selects POSIX or Windows code. Platform headers, calls,
  types, and macros stay behind `src/Platform.hpp` and `os` wrappers.
- A platform-boundary move preserves each existing platform value unless the
  value is part of the requested behavior change.
- Descriptor-rebinding wrappers increment the descriptor epoch. Cached color
  decisions refresh against it. Forks, process groups, filesystems, and processor
  counts also use platform wrappers.

## Completion and language server

- `src/Completion.cpp` drives the `Completion` scan, highlight, syntax, path,
  manpage, and cache sources.
- Completion, highlighting, diagnostics, and koshkit cat share the tolerant
  scanner and semantic roles. Completion, highlighting, and command lookup
  share directory indexes.
- Command completion reads keywords, builtins, bundled utilities, functions,
  aliases, and PATH. `KEYWORD_ENTRIES` is the sole keyword catalog.
- The language server wraps completion in `begin_explicit_completion` and loads
  command documentation lazily. Mood selection checks the shebang, language
  identifier, then extension. `shellscript` selects bash.
- `analyze_ast` optionally collects `analysis_symbol_records` for hover,
  outline, definition, and rename. Ordinary analysis passes null. Assignment
  records cover ordinary, builtin, loop, arithmetic, read, mapfile, getopts,
  printf, and declaration binders. Function records keep body spans.
- Rename uses semantic spans from the open document. Variable edits preserve
  sigils and braces. Command edits require a local function or alias definition.
  Variable names use `word_is_plain_identifier`. Command names use
  `word_is_function_name`.
- `src/EvalVariables.hpp` owns dynamic variables. Completion and hover respect
  the mood. Bare koshkit completion works in the default mood and with the
  koshkit option.

## Diagnostics and storage

- `DiagnosticsCatalog.cpp` owns analysis diagnostics.
  `DiagnosticsDispatch.cpp` owns command dispatch. Other `Diagnostics` sources
  own grouped checks. `SimpleCommand::analyze` builds one `command_lint_input`.
  Whole-script checks run once after `analyze_ast`.
- A leading ShellCheck disable applies to the file. Other disables apply to the
  next complete and-or command. A numeric code suppresses all variants. An exact
  slug suppresses one. Parser and runtime errors remain enabled.
- `SourceLocation` stores 32-bit position, length, and interned source index.
  Syntax nodes store end positions separately. Diagnostics and LINENO share a
  line index.
- Small types stay in light headers. Shared behavior stays on the value type.
  `ArrayList::find` returns `Maybe<usize>`. Membership uses
  `find().has_value()`.
- `Allocator` is one tagged word for pooled heap, bump arena, or fake storage.
  Project code allocates through this API. Only heap storage is freed. Ownership
  queries identify a specific arena. Raw storage is guarded until throwing
  construction succeeds.
- `ArrayList` allocates on first growth and stores 32-bit length and capacity.
  `String` has inline storage and an exact first heap allocation. Use
  `SparseList` for almost-empty member lists.
- Bump arenas register destructors for nontrivial objects. Use
  `is_arena_destructor_noop` only when reachable resources have arena lifetime
  or need no cleanup. Destructor chunks hold 128 records first and 64 KiB later.
- `WordSegment` is 32 bytes on 64-bit targets. Two 32-bit units hold its span,
  kind, and flags. Parsed text, cache data, flattened values, and segment lists
  move into the token arena. Runtime copies own heap storage. Parsed word tokens
  need no destructor record.
- Segment caches hold substitution, arithmetic, folded results, and lifetime
  identities. Parsed caches use syntax or function arenas. Runtime copies use
  the heap. Lifetime checks reject data after arena reuse.

## Build and tests

- Prefer make targets. The top Makefile delegates to `src/Makefile` and supplies
  the processor count. `make MODE=rel` builds `./kosh`. `make MODE=dbg` builds
  `./kosh-dbg` with AddressSanitizer and UndefinedBehaviorSanitizer.
  `make MODE=cov` builds `./kosh-cov`. Debug is the default. `make clean` owns
  removal. Never remove `./kosh` directly.
- `make test` runs main and completion suites. `make bench` runs benchmarks.
  Completion tests require debug. Bound interactive and long-running commands.
- `refill` regenerates goldens. `REFILL` selects source stems. Goldens live
  directly under `test/expected` and have unique names. Read every changed line.
- Make discovers inputs and platform skips. Runners own setup, output,
  comparison, refill, and cleanup. Results are under `.test-work/results`.
  Auxiliary test shell scripts use two-space indentation.
- Run bare `NAME`, `cli_NAME`, and completion targets through `make -C test`.
  Resolve the input and runner first, then pass matching `MODE` and `BIN`
  values. The native runner suppresses incidental diagnostics outside
  `shellcheck_static_*` tests.
- Koshkit rm tests use `--dry-run`. Cleanup uses the system rm after a nonempty
  path check. Bashdiff and mimicrydiff need Bash 5.3 or newer. Set `BASHP` to a
  modern Bash on macOS.

## Workflow

- Read and state the matching guidance before planning, editing, review, prose,
  and commits. Resolve guides separately. Review matching entries in
  [MISTAKES.md](MISTAKES.md) before repeating an action.
- Resolve the configuration directory before expanding an at-sign guidance
  path. Do not assume that guidance is stored below the repository.
- Use parallel read-only research for broad work. Keep scopes disjoint. Validate
  task names and arguments. Wait at least ten seconds.
- Resolve files, tools, services, interpreters, options, streams, test targets,
  cleanup, and expected statuses before use. Recheck CLI options after checkout.
  Run independent probes independently.
- Run compound host probes through an explicitly verified interpreter. Inspect
  an unfamiliar make target recipe before invoking it.
- Bound searches by matches and bytes. Use current nonoverlapping excerpts.
  Use literal ripgrep patterns. Put `--` before dash-leading patterns. Enable
  PCRE2 only when required. Run independent searches independently.
- Quote shell source, use `-c` for source, put `--` before dash-leading operands,
  order redirections from creation to use, and capture status or PIPESTATUS
  before another command changes it. Single-quote literal shell arguments that
  contain backticks.
- Pass generated text through a literal `printf` format when the text contains
  percent conversions.
- Edit with apply_patch. Use one file and concern per patch. Copy current anchors
  and preserve escaping. Inspect failed patches. Reread after formatting or
  concurrent work. Apply only nonempty changes.
- Print the required before-and-after table after each edit batch.

## Implementation

- Search the owner and all callers. Inspect declarations, linkage, enums,
  helpers, containers, packed keys, and formatters. Copy iteration syntax from
  the same type.
- Complete interface, type, field, and container migrations across all uses and
  aggregate initializers before compiling. Check overloads, result types,
  explicit template instantiations, parameter use, widths, local scope, switch
  case scopes, and standard helper declarations.
- Parse internal control markers by their complete validated value. Do not infer
  different values from a shared prefix.
- Keep borrowed views within owner lifetimes. Prove bounds, spans, offsets,
  lengths, optionals, and static evidence. Install restoration guards before
  mutation.
- Handle zero and empty containers before indexed access. Check saturated
  accumulators before subtraction. Narrow values only at command-status edges.
- Order packed fields by alignment and assert important sizes. Use the project
  Bitset or integer masks for fixed boolean tables.
- Inspect the syntax tree before changing control-flow ownership. Cover every
  evaluator and cache path. Use nonconstant fixtures for uncertain branches.
- Remove obsolete branches, fields, flags, and writes. Keep valid comments.

## Make

- Place conditionals and immediate expansions after their variables. Assign
  deferred tools after parse-time probes.
- Preserve argument zero and input origin when parsing MAKEFLAGS. Exclude parent
  bookkeeping and assignments from operands. Recipe exports use macro
  precedence.
- The first ordinary target, including included text, remains the default.
  Repeated makefiles do not replace it.
- Inference preserves explicit rule identity and its own first prerequisite.
  Use POSIX recipe syntax unless syntax mood is under test.

## Validation

- Locate the owner, runner, input, golden, and environment. Run a failing
  regression before a behavior fix. Native tests need a placeholder golden
  before REFILL.
- Rebuild the required mode. Verify platform, mode, and revision when relevant.
  Compile release after changing assertion-only locals.
- Run owners sharing result paths sequentially. Use finite workloads,
  event-based synchronization, bounded polling, and preserved session ids.
- Assert exact streams, statuses, punctuation, source, carets, and log arguments.
  Use distinct fixture names. CLI fixtures cover runtime output. Native fixtures
  also emit lexer and syntax tree output.
- Disable unrelated analysis when a probe isolates runtime behavior.
- Trace native creation and open requests before changing platform access,
  sharing, or security. Verify both payload and status in every direction.
- Normalize platform branches to the same output before changing a shared
  golden.
- Trace each platform child transition before asserting shared subshell state.
- Use explicit koshkit dispatch unless bare lookup is under test. Koshkit rm
  uses `--dry-run`.
- Language server probes initialize first, redirect the emitter, drain output,
  and keep final status. Debug completion and highlighting receive source through
  their debug option without `-c`.
- Poll long work to its final exit. A full suite passes only after every shard
  finishes and no partial failure artifact remains.

## Performance and finish

- Verify executables, services, interfaces, sanitizers, timing, platforms, and
  fallbacks. A Docker client does not prove daemon readiness. Match container
  toolchains to architecture. Force-sign relinked macOS workload binaries.
- Use isolated bounded temporary directories. Verify cleanup targets. Use a
  reviewed patch or Trash when direct removal is filtered. Deinitialize
  temporary worktree submodules before removal.
- Use `MODE=rel` for every performance make command. Clear inherited jobserver
  flags before serial submakes. Resolve benchmark inputs, options, and statuses.
  Enable nonzero handling for expected failures. Run bench through `-c`.
  Benchmark timeouts wrap the measured process directly.
- Validate profilers on a small payload and bound full workloads. Let the
  command runner capture output.
- Record mistakes in [MISTAKES.md](MISTAKES.md) and add one general prevention
  rule here for each distinct cause.
- Format changes. Run focused and full bounded tests. Inspect goldens, the full
  diff, untracked files, and `git diff --check`. Confirm README.md is untouched.
- Verify git identity. Keep commit subjects within the limit and bodies within
  72 columns. Commit locally. Never push or create external artifacts without
  an explicit request.
