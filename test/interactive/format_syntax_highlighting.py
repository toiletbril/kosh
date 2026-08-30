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
with tempfile.NamedTemporaryFile(delete=False, suffix=".sh") as source:
    source.write(b'if test -n "$value"; then echo "$value"; fi\n')
    source_path = source.name

pid, master = pty.fork()
if pid == 0:
    fcntl.ioctl(1, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
    os.environ["TERM"] = "xterm-256color"
    os.environ.pop("NO_COLOR", None)
    os.execv(binary, [binary, "--format", source_path])

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

has_keyword_color = b"\x1b[1;35mif\x1b[0m" in output
has_variable_color = b"\x1b[96m$value\x1b[0m" in output
has_later_keyword_color = b"\x1b[1;35mthen\x1b[0m" in output
has_formatted_structure = b"\r\n\x1b[1;35mthen\x1b[0m\r\n" in output
passed = (
    child_exited_cleanly
    and has_keyword_color
    and has_variable_color
    and has_later_keyword_color
    and has_formatted_structure
)
print("CHILD_EXITED_CLEANLY:", child_exited_cleanly)
print("KEYWORD_COLOR:", has_keyword_color)
print("VARIABLE_COLOR:", has_variable_color)
print("LATER_KEYWORD_COLOR:", has_later_keyword_color)
print("FORMATTED_STRUCTURE:", has_formatted_structure)
print("FORMAT_TERMINAL_HIGHLIGHTING:", passed)
sys.exit(0 if passed else 1)
