#!/usr/bin/env python3
import os
import pty
import select
import sys
import time

binary = sys.argv[1]


def read_until(master, expected, timeout):
    output = b""
    deadline = time.monotonic() + timeout
    while expected not in output and time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], 0.1)
        if master not in readable:
            continue
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        output += chunk
    return output


pid, master = pty.fork()
if pid == 0:
    command = (
        "read -r -s -p SECRET_PROMPT secret; "
        "[ \"$secret\" = hidden-secret ] && printf '\\nSILENT_OK\\n'; "
        "read -r -p VISIBLE_PROMPT visible; "
        "[ \"$visible\" = visible-value ] && printf 'VISIBLE_OK\\n'"
    )
    os.execv(binary, [binary, "--no-init-files", "-c", command])

secret_prompt = read_until(master, b"SECRET_PROMPT", 3)
os.write(master, b"hidden-secret\n")
silent_output = read_until(master, b"VISIBLE_PROMPT", 3)
os.write(master, b"visible-value\n")
visible_output = read_until(master, b"VISIBLE_OK", 3)

waited, status = os.waitpid(pid, 0)
os.close(master)
child_exited_cleanly = waited == pid and os.waitstatus_to_exitcode(status) == 0
secret_was_hidden = b"hidden-secret" not in silent_output
visible_input_was_echoed = b"visible-value" in visible_output
silent_read_succeeded = b"SILENT_OK" in silent_output
visible_read_succeeded = b"VISIBLE_OK" in visible_output
passed = (
    b"SECRET_PROMPT" in secret_prompt
    and child_exited_cleanly
    and secret_was_hidden
    and visible_input_was_echoed
    and silent_read_succeeded
    and visible_read_succeeded
)

print("CHILD_EXITED_CLEANLY:", child_exited_cleanly)
print("SECRET_WAS_HIDDEN:", secret_was_hidden)
print("VISIBLE_INPUT_WAS_ECHOED:", visible_input_was_echoed)
print("SILENT_READ_SUCCEEDED:", silent_read_succeeded)
print("VISIBLE_READ_SUCCEEDED:", visible_read_succeeded)
print("READ_SILENT:", passed)
sys.exit(0 if passed else 1)
