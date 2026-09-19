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
- Prefer names to comments. Owned code carries no comment beyond the license
  notice and the file description header. C and C++ comments use `/* ... */`.
  Brace conditions containing `&&` or `||`, including every branch of an if and
  else-if chain. Separate logical blocks, loops, and returns with blank lines.
- Every project-owned C and C++ file starts with the top-level license
  notice and a detailed description of its concrete responsibilities. The
  description explains why a non-obvious split file exists. Do not add the
  notice to vendored files.
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
- `src/Toiletline.cpp` defines the vendored editor configuration macros itself
  and cannot include `src/Toiletline.hpp`. A declaration that source must see
  belongs in a light header such as `src/ToiletlineHistory.hpp`.
- Each shell loads history once at startup and then keeps a private branch with
  session-local event numbers. Normal prompts and accepted-command appends do
  not import peer records. Only explicit history synchronization replaces the
  branch; cross-shell search uses a separate read-only file snapshot.
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
  and the file creation mask. A fresh evaluator receives replayable shell source
  and framed structured state. Windows duplicates live process handles into the
  authenticated child. The bootstrap owns received handles until evaluator state
  adopts them. Restricted behavior uses one context state. BASHPID identifies
  forked evaluators. `$$` identifies the original shell.
- An asynchronous pipeline job owns and reaps every stage. POSIX stages share a
  process group. The last stage owns status and job output. Stream writes retry
  partial writes and reject zero-length writes while bytes remain.
- A pipe has its writer and its reader in different processes whenever either
  side can exceed the pipe buffer. A deferred stage report is written only after
  every reading stage runs in a child.
- A forked stage closes every descriptor the parent still owns before it reads.
  The close-on-exec flag releases a stage that execs and keeps every descriptor
  of a forked builtin, group, or subshell.
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
- Linux static PIE links without `-Bsymbolic`. Binding libc locale symbols in
  the executable corrupts the locale state used by `localeconv()`.
- Descriptor-rebinding wrappers increment the descriptor epoch. Cached color
  decisions refresh against it. Forks, process groups, filesystems, and processor
  counts also use platform wrappers.
- A routed platform fragment is included into `src/Platform.cpp` and owns no
  object of its own. Compile such a fragment through `Platform.o` of the active
  target and mode. A direct compile of one answers with spurious out-of-scope
  errors.

## Completion and language server

- `src/Completion.cpp` drives the `Completion` scan, highlight, syntax, path,
  manpage, and cache sources.
- Completion, highlighting, diagnostics, and koshkit cat share the tolerant
  scanner and semantic roles. Completion, highlighting, and command lookup
  share directory indexes.
- The interactive loop indexes the working directory before each prompt through
  `utils::warm_directory_index`. The ghost suggestion needs no tab. The ghost
  runs for any non-empty token. A directory is indexed as soon as its slash is
  typed.
- Command completion reads keywords, builtins, bundled utilities, functions,
  aliases, and PATH. `KEYWORD_ENTRIES` is the sole keyword catalog. A `type`
  operand reads the same catalog. An empty operand is answered only in the
  listing mode.
- Static koshkit completion names stay alphabetically sorted.
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

## Editor integrations

- `nvim/lsp.lua` is a paste-in snippet. `vscode` holds a TypeScript extension
  built with npm. `zed` holds a Rust extension built for `wasm32-wasip2`. Each
  directory owns its own install document. None of them is installed by
  `src/Makefile` or packaged by a release workflow.
- Formatting reaches every client through `textDocument/formatting`. The
  `kosh --format` command is the documented fallback for a setup without the
  server.
- A client document selector names only the language identifiers the format
  detector compares against. These are `markdown`, `yaml`, `dockerfile`,
  `makefile`, `json`, and `jsonc`, plus the shell identifiers. Any other
  non-empty identifier makes the whole document parse as shell. A host format
  the detector finds by name is matched by file name in the client.
- Zed receives no identifier for Shell Script. The extension of the file
  selects the mood.
- A client names no transport kind. The stdio kind appends a `--stdio` flag
  that the shell rejects. An executable server with no transport talks over the
  standard streams of its child. Read the argument construction of a client
  library before a transport is selected.
- The client log of a real editor session confirms an integration change. The
  VS Code family writes one file for each extension output channel under its
  own log directory.
- A client resolves the shell from its configured path, then PATH, then its own
  storage. A missing shell is downloaded from the newest release of
  `toiletbril/kosh`. Drafts and prereleases are skipped. An asset is named
  `kosh-<platform>-<processor>-<tag>`. The platform is `darwin`, `linux`, or
  `win32`. The Darwin processor is `aarch64`, and the Linux and Windows
  processor is `amd64`. A Windows asset ends in `.exe`.

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
- `NO_TOILETLINE=1 make` builds the no editor configuration in a separate
  object directory and links the same `./kosh-dbg` path. Rebuild the ordinary
  configuration before the next fixture run.
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
  path check. Bashdiff and mimicrydiff need Bash 5.3 or newer.
  `test/find-modern-bash.sh` selects one from PATH, and `BASHP` overrides that
  choice. An unsuitable `BASHP` fails the suite while a suitable Bash is
  installed.
- Golden comparisons use the resolved host diff on POSIX and koshkit diff on
  Windows through `test/bin/diff`.
- A compat fixture compares its standard output and status exactly. Its error
  output compares only presence, because a kosh diagnostic is worded and located
  its own way. A fixture whose error output is meant to agree byte for byte
  carries `# compat-stderr: exact` on a line of its own.

## Workflow

- Read and state the matching guidance before planning, editing, review, prose,
  and commits. Resolve the configuration directory before expanding an at-sign
  path. Reuse a current read until an edit or external change can make it stale.
  Review matching entries in [MISTAKES.md](MISTAKES.md) before repeating an
  action whose shape has already failed.
- Print the required before-and-after table immediately after each edit batch.
  No tool call may precede a pending table, including a formatter, a build, a
  test, a search, or a guidance reread. After a resumed session, print any
  pending table first, then reread the matching guidance and the current target
  before the first edit.
- Use parallel read-only research for broad work. Keep scopes disjoint between
  agents and the main process until each agent reports. Validate task names and
  arguments. Wait at least ten seconds.
- Resolve files, tools, services, interpreters, options, streams, test targets,
  cleanup, and expected statuses before use. Verify the identity and version of
  a reference interpreter, because a name on PATH can be a symbolic link to the
  shell under test. Recheck CLI options after checkout. Inspect local help
  before using an unfamiliar tool option, and inspect an unfamiliar make target
  recipe before invoking it.
- Resolve an executable in the environment that launches it. Pass its verified
  absolute path when a nested shell can use a different command search path.
  Pass an absolute script path, or set the working directory explicitly, when a
  runner is launched from outside its own directory. A runner that the makefile
  supplies with exported inputs is invoked through its owning target.
- Before branch integration, verify that durable snapshots exist and that the
  integration worktree has no overlapping local changes.
- Inspect the reported source line before diagnosing a shell error.
- Use parallel tool calls for independent reads. Do not join them with shell
  command separators. Run independent probes and searches independently.
- Resolve build artifact paths from the active target before passing them to
  stat, debuggers, or inspection tools.
- Compatibility fixtures use a stable peer-file operand when that operand is
  printed. The runner may pass the main input through different path forms to
  the shell under test and the reference shell.
- Put the explicitly verified interpreter and `-c` in the command text for a
  compound host probe. Do not rely on command-runner shell metadata.
- Reduce a bounded platform probe to one verified command. Confirm its mood,
  option defaults, output order, and final status before writing the golden.
- Run a Bash compatibility probe under `--mood bash`. The default mood follows
  dash where the two shells disagree.
- Redirect a build or a suite into a log file and echo its status, then read the
  log. A pipe reports the status of the last command in the pipeline.
- Resolve redirected log paths from the command runner's working directory;
  a nested tool's `-C` option does not change where the parent shell opens a
  redirection.
- Do not embed direct recursive removal in a probe. Use an accepted cleanup
  owner, or leave a bounded temporary directory for system cleanup.
- Reduce a high-volume fixture to its failing section before enabling shell
  xtrace.
- Bound searches by matches and bytes. Use current nonoverlapping excerpts and
  literal ripgrep patterns. Put every ripgrep option, including `--glob`, before
  the pattern, the separator, and the paths. Give `--glob` a concrete pattern,
  and treat a zero count from a filtered search as unproven until the filter is
  checked. Put `--` before dash-leading patterns. Keep short options separate
  when any option accepts a value. Never pass `-r` to ripgrep, because it names
  the replacement text and rewrites every printed match. Enable PCRE2 only when
  required.
- A command that runs ripgrep must not contain `&&` or `||`. Run an expected
  no-match search as its own command, because its status must not stop later
  checks.
- Resolve every guessed or optional peer path, include root, and wildcard with
  `fd` before passing it to `rg`. Do not type a path merely because a nearby
  file suggests its name. Select `fd` glob mode before passing a wildcard
  pattern.
- Quote shell source, use `-c` for source, put `--` before dash-leading
  operands, and place every option before a `--` separator. Order redirections
  from creation to use. Capture status or PIPESTATUS as the next command after
  the pipeline it describes. Place environment assignments before the command
  that receives them. Single-quote literal shell arguments that contain
  backticks.
- Quote an expansion whose exact spacing an assertion depends on, because word
  splitting collapses a run of blanks in both shells.
- Compare a delimiter-sensitive shell pattern with the literal observed value
  before running it. When a host shell invokes a reference shell with `-c`,
  single-quote the source at the host boundary so the host cannot expand the
  reference variables.
- Keep a shell assignment value on the same logical line as its equals sign.
  Use an explicit continuation before any physical line break.
- Do not use Python, here-documents, sed in-place rewrites, or awk rewrites.
  Shell text tools remain read-only probes. Inspect the complete command
  string, including nested quoted source, for prohibited forms before
  execution. Write a generated file with the write tool or with a `printf`
  redirection. An interpreter chosen for convenience carries its own prohibited
  forms.
- Keep Bash's `$(< file)` fast file read as the substitution's only command.
  Append a sentinel before the read when trailing newlines must be preserved.
  Pass generated text through a literal `printf` format when the text contains
  percent conversions.
- Test a replacement regular expression against exact representative input
  before using it in shared fixture normalization. Keep normalization that must
  visit every line before any sed stage that uses `n`, because the newly read
  line resumes after that command.
- Edit with apply_patch when available. Otherwise, use the exact-edit tool after
  reading the target. Use one file, concern, and operation per path in each
  patch. Copy current anchors and escaping. Anchor patches on unchanged lines
  without escapes when the tool input adds an escaping layer. Restore deleted
  code from the exact diff. Check every multi-file patch boundary before
  applying it. Inspect failed patches, reread after formatting or concurrent
  work, and apply only nonempty changes.
- Do not run a whole-file formatter on a vendored file. Apply narrow formatting
  patches that preserve the surrounding style.
- Before a bulk mechanical rewrite, count every known input form and make the
  transformation idempotent when files may already contain the target form.
- Inspect the formatter diff before validation. Restore changes to unrelated
  files that were clean before the formatter ran.
- Name the active case before each assertion in a compound probe that can stop
  on the first nonzero status.

## Implementation

- Search the owner and all callers. Inspect declarations, linkage, enums,
  helpers, containers, packed keys, formatters, and iteration syntax. Use an
  unrestricted literal source search before deleting a shared symbol, and
  repeat it after the edit.
- Inspect the exact local type before copying member access syntax between
  nearby paths. Copy platform argument types, helper namespaces, and ownership
  transfers from their current declarations before the first compile.
- Complete interface, type, field, and container migrations across all uses and
  aggregate initializers before compiling. Check overloads, result types,
  explicit template instantiations, parameter use, widths, local scope, switch
  case scopes, and standard helper declarations.
- Recheck every break and continue when moving a loop body into a lambda.
- Place a gate on the single producer of the value it governs. A gate added at
  one consumer leaves every other consumer unchanged.
- Read the declaration of an aggregate before writing or reading its positional
  initializer. Read the access label that governs a member before calling it.
- Search for every golden that reads a shared name table before the table gains
  an entry.
- Write and read framed fields in identical order. Review both sides together
  whenever a framed format changes.
- Inspect exact-length stream readers for read-ahead and end-of-file dependence
  before using them on persistent protocol streams.
- Parse internal control markers by their complete validated value. Do not infer
  different values from a shared prefix.
- Verify option defaults against the reference shell. Resolve compact option
  identifiers through the canonical packed table. Declaration order does not
  carry them.
- Measure the reference shell on the exact construct before designing a
  compatibility fix, and design from the measured output alone. The channel
  that carries the source belongs to the construct. Measure through a script
  file, standard input, and `-c` before a rule is generalized across them.
  Measure both shells before a task premise authorizes a source change, because
  the divergence a task describes can already be absent.
- Attribute an observed status to the branch that produced it before that status
  is generalized to another branch of the same builtin.
- Keep borrowed views within owner lifetimes. Prove bounds, spans, offsets,
  lengths, optionals, and static evidence. Install restoration guards before
  mutation. A view copied into heap storage needs a named owner, and the macOS
  sanitizer build reports no leak. A Linux run is the only place such a leak is
  reported.
- Handle zero and empty containers before indexed access. Check saturated
  accumulators before subtraction. Narrow values only at command-status edges.
- Order packed fields by alignment and assert important sizes. Use the project
  Bitset or integer masks for fixed boolean tables.
- Inspect the syntax tree before changing control-flow ownership. Cover every
  evaluator and cache path. Use nonconstant fixtures for uncertain branches.
- Remove obsolete branches, fields, flags, and writes. Keep valid comments.
- Use an explicit branch when a Maybe fallback performs work, because value_or
  evaluates the fallback before the call.
- Verify that macros and declarations are visible in every active build
  configuration before using them.

## Make

- A make target is owned by the makefile of the directory the command runs in.
  Confirm the working directory before a nonzero status is attributed to the
  files under change, and give a root build the repository root as its make
  directory.
- Place conditionals and immediate expansions after their variables. Assign
  deferred tools after parse-time probes.
- A repeated command-line override keeps only its last assignment. Pass a
  multi-valued override as one space-separated value.
- Add variant compiler definitions through `BUILD_DEFINITIONS`. An early
  override of accumulated compiler flags can suppress later platform flags.
- Read the variable that receives an override before a build is retried with it.
  A simple assignment discards the environment value of the same name. The
  environment `CXXFLAGS` reaches both the compile and the link recipe, and its
  text is stamped into a metadata define.
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
- Verify that a capability-gated test ran its active branch. A skip summary is
  not coverage.
- Inspect fixture instrumentation before direct invocation. Match the binary
  mode to every counter or diagnostic hook the fixture requires.
- Rebuild the required mode. Verify platform, mode, and revision when relevant.
  Compile release after changing assertion-only locals. Read the binary a
  failing section actually runs.
- Run a container suite without an allocated terminal. A terminal moves a
  fixture onto its terminal branch and hides the result continuous integration
  reports. A container exec also starts with SIGQUIT ignored, and the startup
  ignore listing reports it. Reproduce a container failure outside the harness
  before it is attributed to the harness.
- A bounded golden fixture runs in a new session without a controlling
  terminal. Job control is unavailable there. A helper that must keep the
  default interrupt disposition runs in the foreground and publishes its own
  process id.
- A helper that moves a process into a new session forks when the caller already
  leads a process group. The new identity is read from the process that owns it,
  and the completion of the work is read from a value that process publishes.
- Force-sign a relinked macOS binary and prove it runs one command before a
  suite is started. An invalid signature kills every invocation with signal 9.
- Run owners sharing result or artifact paths sequentially. Use finite
  workloads, event-based synchronization, bounded polling, and preserved
  session ids.
- Assert exact streams, statuses, punctuation, source, carets, and log
  arguments. Use distinct fixture names. CLI fixtures cover runtime output.
  Native fixtures also emit lexer and syntax tree output. Update the matching
  golden in the same edit when a fixture result label changes.
- Inspect the authoritative formatter order before writing an exact assertion
  for generated text.
- Run ordering assertions against clean state before unrelated entries can
  affect container iteration.
- Remove a focused runner result file before invoking the runner. When direct
  removal is filtered, move it to a bounded temporary backup instead.
- Disable unrelated analysis when a probe isolates runtime behavior, or prefer
  positional operands after `--` in harness-only source.
- Assign a runtime shell variable through source when the test depends on its
  setter. Importing an environment value does not prove that the setter ran.
- Isolate each operation in a hanging command before identifying the cause.
  Repeated probes start with a verified unused or clean path.
- Give each section of an interactive fixture its own temporary directory.
- Read the owner that computes a default before a fixture asserts it.
- Measure a divergence between the two shells for each assertion of a
  compatibility fixture before the fixture is written.
- Run a status or PIPESTATUS capture in the same shell and the same order the
  fixture will use. Read a process-sensitive name with a builtin print in the
  current shell, because a command substitution reports the child identity.
  Target a subshell with `$BASHPID`, because `$$` names the original shell.
- Trace native creation and open requests before changing platform access,
  sharing, or security. Verify both payload and status in every direction.
- Normalize platform branches to the same output before changing a shared
  golden. Trace each platform child transition before asserting shared subshell
  state.
- Use explicit koshkit dispatch unless bare lookup is under test. Koshkit rm
  uses `--dry-run`.
- Verify bundled utility options before using them in fixtures. Prefer shell
  syntax for simple assertions. Keep fixture control flow exhaustive and free
  of unrelated diagnostics.
- Language server probes initialize first, redirect the emitter, drain output,
  and keep final status. Debug completion and highlighting receive source
  through their debug option without `-c`.
- Poll long work to its final exit. A full suite passes only after every shard
  finishes and no partial failure artifact remains.
- Leave the machine idle while a suite runs. The editor recorder tests are timed
  against wall clock, and a concurrent probe makes one of them time out. Read a
  timing failure as load before it is read as a defect.
- Name the target of every suite invocation. A directory make invocation with no
  target runs the first ordinary target of that makefile. The first ordinary
  target of `test/Makefile` is `clean`.

## Performance and finish

- Verify executables, services, interfaces, sanitizers, timing, platforms, and
  fallbacks. A Docker client does not prove daemon readiness. Match container
  toolchains to architecture. Force-sign relinked macOS workload binaries.
- Use isolated bounded temporary directories. Verify cleanup targets. Use a
  reviewed patch or Trash when direct removal is filtered. Deinitialize
  temporary worktree submodules before removal.
- Use `MODE=rel` for every performance make command. Clear inherited jobserver
  flags before serial submakes. Resolve benchmark inputs, options, and statuses.
  Pass additional compiler flags through `USER_CXXFLAGS`. Command-line
  `CXXFLAGS` replaces the project compiler flags. Enable nonzero handling for
  expected failures. Run bench through `-c`. Benchmark timeouts wrap the
  measured process directly.
- Validate profilers on a small payload and bound full workloads. Let the
  command runner capture output.
- Record each mistake in [MISTAKES.md](MISTAKES.md), and add one general
  prevention rule to this file for each distinct cause.
- Format changes. Run focused and full bounded tests. Inspect goldens, the full
  diff, untracked files, and `git diff --check`. Confirm README.md is untouched.
  Check `git ls-files` before staging a path that is absent from status output.
  Inspect vendored formatting rules before formatting a touched vendor file.
- Verify git identity. Keep commit subjects within the limit and bodies within
  72 columns. Pass a commit body through repeated `-m` arguments. Read the
  commit and prose guidance against a drafted body before the commit command is
  composed, and give every consequence clause its own sentence.
- Run `git add` on every new path before that path appears in a commit
  pathspec, and pass an explicit pathspec to every commit, because the index can
  hold a path that another worker staged in the same working copy. A status
  reading goes stale as soon as another command runs.
- Commit locally. Never push or create external artifacts without an explicit
  request.
