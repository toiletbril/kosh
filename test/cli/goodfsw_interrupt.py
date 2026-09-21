#!/usr/bin/env python3

import os
import signal
import subprocess
import time


process = subprocess.Popen(
    [
        os.environ["BIN"],
        "-c",
        "koshkit goodfsw -r -l 0.05 " + os.environ["INTERRUPT_ROOT"],
    ],
    stdout=subprocess.DEVNULL,
    stderr=subprocess.DEVNULL,
)
time.sleep(0.1)
process.send_signal(signal.SIGINT)
try:
    status = process.wait(timeout=10)
except subprocess.TimeoutExpired:
    process.kill()
    process.wait()
    status = 124
print(status)
