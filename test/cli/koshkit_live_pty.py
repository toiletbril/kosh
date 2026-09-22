#!/usr/bin/env python3
"""Bounded PTY and redirected probes for EvilIO and EvilPS live output."""

import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


def run_pty(binary, command, key=None):
    pid, fd = pty.fork()
    if pid == 0:
        os.execv(binary, [binary, "-Q", "-c", command])

    def resize(columns, rows):
        fcntl.ioctl(fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, columns, 0, 0))

    resize(100, 30)
    output = bytearray()
    resized = False
    sent_key = False
    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
        if not resized and len(output) > 1000:
            resize(45, 10)
            resized = True
            if key is not None and not sent_key:
                os.write(fd, key)
                sent_key = True
            time.sleep(0.15)
        if key is not None and not sent_key and len(output) > 1500:
            os.write(fd, key)
            sent_key = True

    if resized:
        os.kill(pid, signal.SIGINT)
    else:
        os.kill(pid, signal.SIGKILL)
    drain_deadline = time.monotonic() + 1.0
    while time.monotonic() < drain_deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        output.extend(chunk)
    _, status = os.waitpid(pid, 0)
    all_parts = bytes(output).split(b"ctrl+c to exit")
    # The final part contains terminal cleanup after the last frame, not a frame.
    frame_parts = all_parts[1:-1]
    blank_counts = [part.count(b"\r\n\r\n") for part in frame_parts]
    return {
        "status": os.waitstatus_to_exitcode(status),
        "resized": resized,
        "frames": bytes(output).count(b"ctrl+c to exit"),
        "controls": b"ctrl+c to exit" in output,
        "ansi": b"\x1b[" in output,
        "alternate_enter": b"\x1b[?1049h" in output,
        "alternate_leave": b"\x1b[?1049l" in output,
        "cursor_hide": b"\x1b[?25l" in output,
        "cursor_show": b"\x1b[?25h" in output,
        "blank_separator": bool(frame_parts) and all(
            count == 1 for count in blank_counts
        ),
        "sort_cycle": all(marker in output for marker in (
            b"SORT tree", b"SORT name", b"SORT pid", b"SORT cpu",
            b"SORT memory")),
        "search_query": b"SEARCH /1" in output,
    }


def run_redirected(binary, command):
    with tempfile.TemporaryFile() as output:
        process = subprocess.Popen([binary, "-Q", "-c", command],
                                   stdout=output, stderr=subprocess.STDOUT)
        time.sleep(0.35)
        process.send_signal(signal.SIGINT)
        status = process.wait(timeout=3.0)
        output.seek(0)
        data = output.read()
    return {
        "status": status,
        "lines": data.count(b"\n"),
        "ansi": b"\x1b[" in data,
        "controls": b"ctrl+c to exit" in data,
    }


def check(name, result, requirements):
    failed = [key for key, expected in requirements.items()
              if result.get(key) != expected]
    if failed:
        print("%s FAIL %s result=%r" % (name, ",".join(failed), result))
        return False
    print("%s PASS %r" % (name, result))
    return True


def main():
    if sys.platform != "linux":
        print("live PTY probes: skipped (requires Linux)")
        return 0
    binary = os.environ.get("BIN")
    if not binary:
        print("BIN is required", file=sys.stderr)
        return 2

    ok = True
    for name, command, key in (
        ("evilio-pty", "koshkit --color never evilio --ps --live=0.05 "
         "--cumulative=0.1", None),
        ("evilps-pty", "koshkit --color never evilps --cpu --live=0.05 "
         "--cumulative=0.1 -1", b"s\n" * 5 + b"/1\n"),
    ):
        result = run_pty(binary, command, key if name == "evilps-pty" else None)
        requirements = {"status": 130, "resized": True,
                        "controls": True, "ansi": True,
                        "alternate_enter": True,
                        "alternate_leave": True,
                        "cursor_hide": True,
                        "cursor_show": True,
                        "blank_separator": True}
        if key is not None:
            requirements["sort_cycle"] = True
            requirements["search_query"] = True
        ok &= check(name, result, requirements)
        if result["frames"] < 2:
            print("%s FAIL fewer than two live frames" % name)
            ok = False

    for name, command in (
        ("evilio-redirected", "koshkit --color never evilio --ps --live=0.05 "
         "--cumulative=0.1"),
        ("evilps-redirected", "koshkit --color never evilps --cpu --live=0.05 "
         "--cumulative=0.1 -1"),
    ):
        result = run_redirected(binary, command)
        ok &= check(name, result, {"status": 130, "ansi": False,
                                   "controls": True})
        if result["lines"] < 2:
            print("%s FAIL fewer than two redirected frames" % name)
            ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
