# Test suite guidance

## Goal

The test suite preserves behavior with the fewest source files and target
process launches that can express the contract. A new regression normally
extends an existing owner. A new test file is the last choice.

The debug test step should finish within 180 seconds. It must finish within 300
seconds. A change that adds measurable runtime should remove equivalent cost or
explain why the process boundary is required.

## Choose the harness

Place a test in the cheapest harness that can express its behavior.

- `kosh/` owns native shell syntax, evaluation, diagnostics, builtins, and
  koshkit utility behavior that can run in one shell process. Each source uses
  the matching `expected/<name>.out` golden.
- `cli/` owns executable flags, argument zero, startup processing, named script
  and standard input modes, fresh-process state, process replacement, signals,
  jobs, terminal behavior, and real deadlines. Each source uses the matching
  `expected/<name>.out` golden.
- `sh/` owns POSIX compatibility against dash. Add a case to an existing
  feature family whenever possible.
- `bash/` owns Bash compatibility against Bash 5.3 or newer. Add a case to an
  existing feature family whenever possible.
- `completion/` owns completion candidates and completion policy. Each source
  uses the matching `expected/<name>.out` golden.
- `highlight/` owns debug highlighting output. Each source uses the matching
  `expected/<name>.out` golden.
- `interactive/` owns checks that require a controlling terminal or byte-level
  terminal inspection.
- `build/` owns build and host-script behavior that does not test the shell
  runtime.
- `bench/` owns performance workloads. Behavioral regressions belong in another
  harness.
- `data/` owns shared goldens. A golden is data and does not become a second
  behavior owner.

Do not use `cli/` only because a shell driver is convenient. Runtime mood and
option changes should occur inside one native test with `set --mood`, `set -o`,
or a subshell. Keep a CLI process boundary only when the boundary is part of the
contract.

## Keep harness logic in runners

Make discovers inputs, declares direct target dependencies, and keeps platform
skip lists. A Make recipe launches the runner for its harness. A runner never
invokes Make.

Each harness has one `run-<harness>-test.sh` file when one process model can
serve all of its cases. The runner creates the required directories, launches
the test process, captures output, compares the flat golden, and removes its
temporary output. Refill is a mode of the same runner. A second script must not
copy a timeout list, argument parser, golden comparison, or cleanup path.

`run-test-suite.sh` bounds parallel workers and starts each harness runner.
`run-bounded-cli-golden.sh` owns process-tree timeouts for CLI cases that can
block. A bounded golden is launched under `/bin/sh`, which is busybox ash on the
Alpine image, so its body is restricted to POSIX shell. A bash extension such as
`$(<file)` or a here-string is silently empty there while the exit status still
looks correct. `run-refill.sh` selects the normal harness runners in refill mode.

Auxiliary shell scripts use two-space indentation. They receive configuration
through exported test variables. They do not reconstruct Make variables or
launch recursive Make processes.

## Keep one owner

Search all test directories before adding a case. Search by builtin name,
utility name, diagnostic text, flag, syntax form, and expected output.

A builtin or koshkit utility has one canonical behavior owner. Extend that file
when the new case exercises the same component. Use names such as
`<builtin>.kosh` and `koshkit_<utility>.kosh` for native owners. A legacy CLI
owner may remain canonical when its behavior inherently needs process control.

A compatibility, completion, highlighting, or terminal test may exercise the
same command only when it owns a different contract. Add the case to the
existing family in that harness. Do not copy the native behavior matrix into
the compatibility suite.

An integration test may call several builtins or utilities as setup or probes.
Those incidental calls do not make it another behavior owner. Keep only the
interaction assertion in the integration test.

Merge files when they share an owner and can run with the same shell state.
Reset options, traps, variables, the working directory, the umask, file
descriptors, and temporary files between merged compatibility sections.

Keep a case separate when concatenation would change the behavior being tested.
Top-level exit, `errexit`, fatal parsing, an EXIT trap, process replacement,
signals, job control, and terminal ownership commonly require isolation.

The `_1.sh`, `_1.bash`, and `_1.out` forms are accepted alternatives for a
single primary test. They are not additional discovered tests. Add an
alternative only for a documented platform or reference-shell difference.

## Write stable tests

Assert behavior, status, diagnostics, and side effects. Do not snapshot literal
help output. Test help only when parsing, routing, completion, an error, or a
special literal operand is the contract.

Keep output small and deterministic. Print short section labels when several
cases share one file. Avoid environment dumps, unordered directory listings,
host paths, process identifiers, timestamps, and full command output when a
focused assertion is sufficient.

Use paths below `$TEST_TEMP_DIRECTORY`. Prefer one fixed directory for the
test. The `bin/mktemp` shim is available when uniqueness is part of the test.
It uses host filesystem operations and does not launch the tested shell.
`TEST_MKTEMP_DIRECTORY` gives the shim an absolute native allocation root.

Do not add fixed sleeps for synchronization. Use a ready marker, pipe,
process-state check, or bounded polling loop. Retain a bounded timeout for a
test that can block.

Every koshkit rm invocation uses `--dry-run`. Temporary directory cleanup uses
the system rm behind a nonempty path guard.

Use `TEST_PATH_SEPARATOR`, `TEST_NULL_DEVICE`, `TEST_PATH_ENVIRONMENT_NAME`,
`TEST_SYSTEM_PATH`, and `TEST_UNAME_DIRECTORY` for portable goldens. Add a
Windows skip in `Makefile` only when the backend cannot express the contract.
Do not hide a portable test behind a platform skip.

Native tests run with `-AER`. Normalize mood, diagnostics, warnings, and shell
options inside a moved or merged case when it relied on another initial state.

## Preserve coverage while merging

Move the source section and its expected output together. Preserve the order of
observable output. Compare the old and new sections before deleting the old
source and golden.

Do not merge only by filename similarity. Check termination, final status,
platform skips, alternate goldens, traps, working directory changes, and shared
state first.

Compatibility files compare one reference-shell result with explicit mood and
mimicry execution. Do not add another runner or repeat the reference launch.

A source and its golden use the same stem. No source may lack its golden, and no
golden may remain after its source is removed. POSIX and Bash compatibility
files do not use repository goldens.

Every golden lives directly in `expected/`. Do not create a subdirectory below
`expected/`. Test names must remain unique across every golden-backed harness.

## Validate the change

Regenerate only the affected native and CLI goldens.

```sh
make -C test refill REFILL='name another_name'
```

Read every regenerated golden. Refill records output without deciding whether
the output is correct.

Run the focused owner through its existing Make target. Native targets use the
test name. CLI, completion, highlight, and build targets use their directory
prefix.

```sh
make -C test name
make -C test cli_name
make -C test completion_name
make -C test highlight_name
make -C test build_name
```

Run full validation from the repository root.

```sh
make test
```

Do not use `make -C test test` for full validation. Inspect the complete diff,
run `git diff --check`, and confirm that README.md remains untouched.

## Final checklist

- The case extends the existing canonical owner when one exists.
- The selected harness is the cheapest harness that preserves the contract.
- A builtin or utility does not gain a second behavior owner.
- Similar native, POSIX, and Bash cases were compared and deduplicated.
- Each required process launch and timeout is part of the behavior under test.
- The output is focused, deterministic, and free of literal help snapshots.
- Temporary paths and platform behavior follow the shared test variables.
- Every changed golden was regenerated narrowly and read completely.
- Focused validation and `make test` pass within the runtime limits.
