#!/usr/bin/env python3
import os
import signal
import subprocess
import sys
import threading
import time


def reset_sigint() -> None:
    signal.signal(signal.SIGINT, signal.SIG_DFL)


def main() -> int:
    binary, directory, fifo = sys.argv[1:]
    command = [binary, "--no-traces", "-c", "koshkit grep . grep-first grep"]
    writer_fd = None

    def open_writer() -> None:
        nonlocal writer_fd
        writer_fd = os.open(fifo, os.O_WRONLY)

    writer = threading.Thread(target=open_writer, daemon=True)
    writer.start()
    process = subprocess.Popen(
        command,
        cwd=directory,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        preexec_fn=reset_sigint,
    )
    writer.join(timeout=2)
    time.sleep(0.1)
    process.send_signal(signal.SIGINT)
    output, _ = process.communicate(timeout=5)
    if writer_fd is not None:
        os.close(writer_fd)

    status = 130 if process.returncode == -signal.SIGINT else process.returncode
    if status == 130 and "grep-first:" in output:
        print("grep-interrupt=preserved")
    else:
        print("grep-interrupt=missing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
