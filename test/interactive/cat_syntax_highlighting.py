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
with tempfile.NamedTemporaryFile(delete=False) as source:
    source.write(
        b'#!/usr/bin/env bash\r\n'
        b'RESOLVED_VARIABLE=$PATH\r\n'
        b'for LOOP_VARIABLE in one; do echo "$LOOP_VARIABLE"; done\r\n'
        b'echo "$PATH" ${PATH} $((PATH + 1))\r\n'
        b'echo "$CAT_UNSET_VARIABLE" ${CAT_BRACED_UNSET} '
        b'$((CAT_ARITHMETIC_UNSET + 1))\r\n'
        b'if true && false || true; then : >out; fi\r\n'
        b'echo "styled"\r\n'
        b'finish() { :; }\r\n'
        b'finish\r\n'
        b'missing_command\r\n'
        b'ech'
    )
    source_path = source.name

pid, master = pty.fork()
if pid == 0:
    fcntl.ioctl(1, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    os.environ["TERM"] = "xterm-256color"
    os.environ.pop("NO_COLOR", None)
    os.environ.pop("CAT_UNSET_VARIABLE", None)
    os.environ.pop("CAT_BRACED_UNSET", None)
    os.environ.pop("CAT_ARITHMETIC_UNSET", None)
    command = "koshkit cat --syntax-highlighting '%s'" % source_path
    os.execv(binary, [binary, "-c", command])

output = b""
deadline = time.monotonic() + 10
while time.monotonic() < deadline:
    readable, _, _ = select.select([master], [], [], 0.5)
    if master not in readable:
        continue
    try:
        chunk = os.read(master, 4096)
    except OSError:
        break
    if not chunk:
        break
    output += chunk

os.close(master)
child_exited_cleanly = False
reap_deadline = time.monotonic() + 2
while time.monotonic() < reap_deadline:
    waited, status = os.waitpid(pid, os.WNOHANG)
    if waited == pid:
        child_exited_cleanly = os.waitstatus_to_exitcode(status) == 0
        break
    time.sleep(0.02)
else:
    os.kill(pid, signal.SIGKILL)
    os.waitpid(pid, 0)
os.unlink(source_path)
command_forms_are_resolved = {
    "function": b"\x1b[34mfinish\x1b[0m" in output,
    "unknown": b"\x1b[34mmissing_command\x1b[0m" in output,
    "partial": b"\x1b[34mech\x1b[0m" in output,
}
variable_forms_are_resolved = {
    "assignment": b"\x1b[96mRESOLVED_VARIABLE\x1b[0m" in output,
    "loop": b"\x1b[96mLOOP_VARIABLE\x1b[0m" in output,
    "dollar": b"\x1b[96m$PATH\x1b[0m" in output,
    "braced": b"\x1b[96m${PATH}\x1b[0m" in output,
    "arithmetic": b"\x1b[96mPATH\x1b[0m" in output,
    "unset-dollar": b"\x1b[96m$CAT_UNSET_VARIABLE\x1b[0m" in output,
    "unset-braced": b"\x1b[96m${CAT_BRACED_UNSET}\x1b[0m" in output,
    "unset-arithmetic": b"\x1b[96mCAT_ARITHMETIC_UNSET\x1b[0m" in output,
}
syntax_forms_use_requested_palette = {
    "keyword": b"\x1b[1;35mif\x1b[0m" in output,
    "and": b"\x1b[1;35m&&\x1b[0m" in output,
    "or": b"\x1b[1;35m||\x1b[0m" in output,
    "redirection": b"\x1b[1;35m>\x1b[0m" in output,
}
has_no_underline = b"4:3" not in output
passed = (
    child_exited_cleanly
    and b"\x1b[" in output
    and all(command_forms_are_resolved.values())
    and all(variable_forms_are_resolved.values())
    and all(syntax_forms_use_requested_palette.values())
    and has_no_underline
)
print("CHILD_EXITED_CLEANLY:", child_exited_cleanly)
print("COMMAND_FORMS_RESOLVED:", command_forms_are_resolved)
print("VARIABLE_FORMS_RESOLVED:", variable_forms_are_resolved)
print("SYNTAX_FORMS_USE_REQUESTED_PALETTE:", syntax_forms_use_requested_palette)
print("NO_UNDERLINE:", has_no_underline)
print("TERMINAL_HIGHLIGHTING:", passed)
sys.exit(0 if passed else 1)
