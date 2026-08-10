#!/usr/bin/env python3
import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time

binary = sys.argv[1]


def read_until_idle(master, timeout, required_output=None):
    output = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], 0.1)
        if master not in readable:
            if output and (required_output is None or required_output in output):
                break
            continue
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        output += chunk
    return output


def render_highlighted_line(term):
    styled_final_token = b"\x1b[1;91mthen\x1b[0m"

    with tempfile.TemporaryDirectory() as directory:
        pid, master = pty.fork()
        if pid == 0:
            fcntl.ioctl(1, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 240, 0, 0))
            os.environ["TERM"] = term
            os.environ["HOME"] = directory
            os.environ["KOSH_HISTORY"] = os.path.join(directory, "history")
            os.environ.pop("NO_COLOR", None)
            os.environ.pop("PALETTE_UNSET", None)
            os.execv(binary, [binary, "--norc"])

        read_until_idle(master, 3)
        os.write(
            master,
            b'if true && false; then echo "$PATH" "$PALETTE_UNSET" >out; fi; '
            b"echo ./underline_missing_path/; underline_missing_command; then",
        )
        output = read_until_idle(master, 2, styled_final_token)
        os.write(master, b"\x03exit\n")
        read_until_idle(master, 1)
        os.close(master)

        deadline = time.monotonic() + 2
        child_exited_cleanly = False
        while time.monotonic() < deadline:
            waited, status = os.waitpid(pid, os.WNOHANG)
            if waited == pid:
                child_exited_cleanly = os.waitstatus_to_exitcode(status) == 0
                break
            time.sleep(0.02)
        else:
            os.kill(pid, signal.SIGKILL)
            os.waitpid(pid, 0)

        return output, child_exited_cleanly


generic_output, generic_child_exited_cleanly = render_highlighted_line(
    "xterm-256color"
)
kitty_output, kitty_child_exited_cleanly = render_highlighted_line("xterm-kitty")
underline_sgr_fragments = (
    b"\x1b[4m",
    b"\x1b[4;",
    b"\x1b[4:",
    b";4m",
    b";4;",
    b";4:",
    b"\x1b[58:",
    b";58:",
)
generic_has_no_underline = all(
    fragment not in generic_output for fragment in underline_sgr_fragments
)
kitty_has_no_underline = all(
    fragment not in kitty_output for fragment in underline_sgr_fragments
)
shared_palette_tokens = (
    b"\x1b[1;35mif\x1b[0m",
    b"\x1b[1;35m&&\x1b[0m",
    b"\x1b[1;35m>\x1b[0m",
    b"\x1b[96m$PATH\x1b[0m",
)
shared_palette_is_applied = all(
    all(token in output for token in shared_palette_tokens)
    for output in (generic_output, kitty_output)
)
generic_unset_is_red = b"\x1b[91m$PALETTE_UNSET\x1b[0m" in generic_output
kitty_unset_is_red = b"\x1b[91m$PALETTE_UNSET\x1b[0m" in kitty_output
generic_invalid_path_is_red = (
    b"\x1b[91munderline_missing_path\x1b[0m" in generic_output
)
kitty_invalid_path_is_red = (
    b"\x1b[91munderline_missing_path\x1b[0m" in kitty_output
)
generic_unknown_is_red = (
    b"\x1b[91munderline_missing_command\x1b[0m" in generic_output
)
kitty_unknown_is_red = (
    b"\x1b[91munderline_missing_command\x1b[0m" in kitty_output
)
generic_invalid_syntax_is_bold_red = (
    b"\x1b[1;91mthen\x1b[0m" in generic_output
)
kitty_invalid_syntax_is_bold_red = (
    b"\x1b[1;91mthen\x1b[0m" in kitty_output
)
passed = (
    generic_child_exited_cleanly
    and kitty_child_exited_cleanly
    and generic_has_no_underline
    and kitty_has_no_underline
    and shared_palette_is_applied
    and generic_unset_is_red
    and kitty_unset_is_red
    and generic_invalid_path_is_red
    and kitty_invalid_path_is_red
    and generic_unknown_is_red
    and kitty_unknown_is_red
    and generic_invalid_syntax_is_bold_red
    and kitty_invalid_syntax_is_bold_red
)
print("GENERIC_CHILD_EXITED_CLEANLY:", generic_child_exited_cleanly)
print("KITTY_CHILD_EXITED_CLEANLY:", kitty_child_exited_cleanly)
print("GENERIC_NO_UNDERLINE:", generic_has_no_underline)
print("KITTY_NO_UNDERLINE:", kitty_has_no_underline)
print("SHARED_PALETTE_IS_APPLIED:", shared_palette_is_applied)
print("GENERIC_UNSET_IS_RED:", generic_unset_is_red)
print("KITTY_UNSET_IS_RED:", kitty_unset_is_red)
print("GENERIC_INVALID_PATH_IS_RED:", generic_invalid_path_is_red)
print("KITTY_INVALID_PATH_IS_RED:", kitty_invalid_path_is_red)
print("GENERIC_UNKNOWN_IS_RED:", generic_unknown_is_red)
print("KITTY_UNKNOWN_IS_RED:", kitty_unknown_is_red)
print("GENERIC_INVALID_SYNTAX_IS_BOLD_RED:", generic_invalid_syntax_is_bold_red)
print("KITTY_INVALID_SYNTAX_IS_BOLD_RED:", kitty_invalid_syntax_is_bold_red)
print("TERM_UNDERLINE_POLICY:", passed)
sys.exit(0 if passed else 1)
