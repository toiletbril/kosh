#!/usr/bin/env python3

import os
import signal
import subprocess
import tempfile
import time


root = tempfile.mkdtemp(prefix="goodfsw-rss-")
for index in range(128):
    open(os.path.join(root, "file-%d" % index), "w").close()

process = subprocess.Popen(
    [os.environ["BIN"], "-c", "koshkit goodfsw -r -l 0.02 " + root],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
samples = []
for _ in range(20):
    try:
        with open("/proc/%d/status" % process.pid) as status_file:
            for line in status_file:
                if line.startswith("VmRSS:"):
                    samples.append(int(line.split()[1]))
                    break
    except FileNotFoundError:
        break
    time.sleep(0.1)

process.send_signal(signal.SIGINT)
status = process.wait(timeout=10)
if status != 130 or not samples or max(samples) - min(samples) > 32768:
    print("growth")
else:
    print("stable")
