Interactive pty harnesses
=========================

These checks cover behavior that needs a controlling terminal.

Build with `make MODE=dbg`, then run a script with an optional binary path. The
main test suite discovers every `interactive/*.py` script when Python is
available.

`cat_syntax_highlighting.py` checks terminal syntax colors for shitbox cat.
`long_warning_window.py` checks clipped diagnostics and caret alignment.
`mimic_terminal_handoff.py` checks foreground handoff and prompt recovery.
`underline_term_support.py` checks terminal underline capability handling.
