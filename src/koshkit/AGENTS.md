# Koshkit utility guidance

## Reuse the utility framework

Search `src/koshkit`, `Koshkit.hpp`, and `Koshkit.cpp` before adding behavior.
Existing flag types, parsers, path helpers, stream helpers, and diagnostics
should own shared behavior.

Every utility declares flags with `FLAG_LIST_DECL`, `FLAG`, and the shared flag
types. `REGISTER_KOSHKIT_UTIL_FLAGS` publishes the list for execution, help,
and completion. `parse_util_operands` handles flags, `--`, and operand
locations. `KOSHKIT_SHOW_HELP_AND_RETURN` handles help.

Do not write a local option scanner for ordinary short flags, long flags,
bundled flags, flag arguments, `--`, or operand locations. Extend
`parse_util_operands` when a missing grammar applies to utilities. A utility may
use a specialized parser only when its documented operand grammar cannot be
represented by the shared parser.

Use `report_soft_koshkit_error` for recoverable utility diagnostics. Use
`ExecContext` for standard streams and output. File operations accept `Path`.
Platform operations pass through the `os` wrappers.

## Register one utility

Add the kind, name entry, switch case, and struct declaration in
`Koshkit.hpp`. Keep the count derived from the last enum value. The source file
belongs directly below `src/koshkit`. The source Makefile discovers it
automatically.

Do not invoke a host utility to implement bundled behavior. Do not duplicate a
reader, writer, traversal, numeric parser, signal parser, or process helper.
Multi-file utilities preserve the established aggregate status and interrupt
behavior.

## Preserve one behavior owner

The canonical native test is `test/kosh/koshkit_<utility>.kosh` unless the
behavior requires a real executable boundary. Add cases to that owner. Other
utilities may appear as setup commands without becoming behavior owners.
Literal help output has no golden.

A new utility updates the root `AGENTS.md`, `docs/kosh.1`, and
`completions/kosh.bash`. Add its name to the existing koshkit completion data.
Registered flags feed runtime completion.
