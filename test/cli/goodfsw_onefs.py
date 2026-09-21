#!/usr/bin/env python3

import os
import signal
import subprocess
import tempfile
import time


root = tempfile.mkdtemp(prefix="goodfsw-onefs-")
mountpoint = os.path.join(root, "mount")
os.mkdir(mountpoint)
mounted = subprocess.run(
    ["mount", "-t", "tmpfs", "-o", "size=1m", "goodfsw-test", mountpoint],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
).returncode == 0
if not mounted:
    print("skipped")
    raise SystemExit(0)


def run_watch(one_file_system):
    flags = ["-r", "-1", "-l", "0.05"]
    if one_file_system:
        flags.append("-x")
    source = "koshkit goodfsw " + " ".join(flags) + " " + root
    process = subprocess.Popen(
        [os.environ["BIN"], "-c", source],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    time.sleep(0.15)
    with open(os.path.join(mountpoint, "mounted-file"), "w"):
        pass
    try:
        output, _ = process.communicate(timeout=2)
    except subprocess.TimeoutExpired:
        process.send_signal(signal.SIGINT)
        output, _ = process.communicate(timeout=10)
    return output


try:
    isolated_output = run_watch(True)
    os.unlink(os.path.join(mountpoint, "mounted-file"))
    traversed_output = run_watch(False)
    isolated = "isolated" if "mounted-file" not in isolated_output else "crossed"
    traversed = "traversed" if "mounted-file" in traversed_output else "hidden"
    print(isolated + "/" + traversed)
finally:
    subprocess.run(["umount", mountpoint], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL)
