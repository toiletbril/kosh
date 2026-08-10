#!/usr/bin/env python3
import os
import pty
import select
import sys
import tempfile
import time

here = os.path.dirname(os.path.abspath(__file__))
binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "kosh-dbg")
with tempfile.NamedTemporaryFile("w", suffix=".bash", delete=False) as script:
    script.write("#!/bin/bash\necho MIMIC_RAN_OK\n")
    script_path = script.name
os.chmod(script_path, 0o755)

pid, master = pty.fork()
if pid == 0:
    os.execv(binary, [binary, "-I", "-i", "--mood", "bash"])

os.write(master, ("stty tostop\n%s\necho HANDOFF_PROMPT_BACK\nexit\n" % script_path).encode())
output = b""
deadline = time.monotonic() + 15
while time.monotonic() < deadline:
    readable, _, _ = select.select([master], [], [], 0.5)
    if master in readable:
        try:
            chunk = os.read(master, 4096)
        except OSError:
            break
        if not chunk:
            break
        output += chunk
    waited, _ = os.waitpid(pid, os.WNOHANG)
    if waited == pid:
        pid = 0
        break

os.close(master)
if pid != 0:
    os.kill(pid, 9)
    os.waitpid(pid, 0)
os.unlink(script_path)

text = output.decode(errors="replace")
ran = "MIMIC_RAN_OK" in text
prompt_returned = "HANDOFF_PROMPT_BACK" in text
passed = ran and prompt_returned
print("MIMIC_RAN_OK:", ran)
print("PROMPT_BACK:", prompt_returned)
print("RESULT:", "PASS" if passed else "FAIL")
sys.exit(0 if passed else 1)
