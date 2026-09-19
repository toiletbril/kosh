#!/usr/bin/env python3
"""The interactive calc prompt keeps its own history file.

A calc session swaps the editor history to the calc file on entry and back on
leave. A shell command never lands in the calc file and a calc expression never
lands in the shell file. Both files are named through the environment. The
session never reaches the real home directory.
"""

import os
import pty
import select
import signal
import sys
import tempfile
import time

binary = os.path.abspath(sys.argv[1])


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


def read_file(path):
    try:
        with open(path, "rb") as handle:
            return handle.read()
    except OSError:
        return b""


def run_session(directory, shell_path, calc_path):
    pid, master = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["HOME"] = directory
        os.environ["KOSH_WELCOME"] = ""
        os.environ["KOSH_HISTORY_FILE"] = shell_path
        os.environ["KOSH_CALC_HISTORY"] = calc_path
        os.chdir(directory)
        os.execv(binary, [binary, "-Q", "-i"])

    read_until_idle(master, 3)
    for line in (b"echo shell-one\n", b"koshkit calc -i\n", b"2+2\n"):
        os.write(master, line)
        read_until_idle(master, 1)

    with open(shell_path, "ab") as handle:
        handle.write(b"peer-event\n")

    for line in (b"111*3\n", b"\x04", b"echo shell-two\n",
                 b"history > branch-history\n"):
        os.write(master, line)
        read_until_idle(master, 1)

    os.write(master, b"exit\n")
    read_until_idle(master, 3)
    os.close(master)

    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        waited, _ = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            break
        time.sleep(0.02)
    else:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)


def main():
    with tempfile.TemporaryDirectory() as directory:
        shell_path = os.path.join(directory, "shell-history")
        calc_path = os.path.join(directory, "calc-history")
        branch_path = os.path.join(directory, "branch-history")
        run_session(directory, shell_path, calc_path)

        shell_history = read_file(shell_path)
        calc_history = read_file(calc_path)
        branch_history = read_file(branch_path)

    shell_history_keeps_shell_commands = (
        b"echo shell-one" in shell_history
        and b"koshkit calc -i" in shell_history
        and b"echo shell-two" in shell_history
    )
    shell_history_omits_calc_expressions = (
        b"2+2" not in shell_history and b"111*3" not in shell_history
    )
    calc_history_keeps_calc_expressions = (
        b"2+2" in calc_history and b"111*3" in calc_history
    )
    calc_history_omits_shell_commands = (
        b"echo shell-one" not in calc_history
        and b"echo shell-two" not in calc_history
    )
    shell_branch_omits_peer_commands = b"peer-event" not in branch_history

    results = {
        "SHELL_HISTORY_KEEPS_SHELL_COMMANDS": (
            shell_history_keeps_shell_commands
        ),
        "SHELL_HISTORY_OMITS_CALC_EXPRESSIONS": (
            shell_history_omits_calc_expressions
        ),
        "CALC_HISTORY_KEEPS_CALC_EXPRESSIONS": (
            calc_history_keeps_calc_expressions
        ),
        "CALC_HISTORY_OMITS_SHELL_COMMANDS": calc_history_omits_shell_commands,
        "SHELL_BRANCH_OMITS_PEER_COMMANDS": shell_branch_omits_peer_commands,
    }

    for name, value in results.items():
        print("%s: %s" % (name, value))

    passed = all(results.values())
    print("CALC_HISTORY:", passed)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
