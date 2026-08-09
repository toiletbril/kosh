#!/usr/bin/env python3
import fcntl
import os
import pty
import re
import select
import signal
import struct
import sys
import tempfile
import termios
import time

here = os.path.dirname(os.path.abspath(__file__))
binary = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "shit-dbg")
column_count = 80
wide_character = "\u754c"
source_line = 'test "%s" = $UNSET %s' % (
    "a" * 119 + wide_character,
    "b" * 120,
)
warning_lines = [
    source_line,
    "if test x = $SECOND_UNSET && true; then : >diagnostic-output; fi",
    "test y = $THIRD_UNSET",
    "diagnostic_missing_command argument",
    "ech",
]
source = "\n" * 20000 + "\n".join(warning_lines)
with tempfile.NamedTemporaryFile(delete=False) as log:
    log_path = log.name

pid, master = pty.fork()
if pid == 0:
    fcntl.ioctl(1, termios.TIOCSWINSZ, struct.pack("HHHH", 24, column_count, 0, 0))
    os.environ["TERM"] = "xterm-256color"
    os.environ.pop("NO_COLOR", None)
    os.execv(
        binary,
        [
            binary,
            "-X",
            "all",
            "--debug-logging-file",
            log_path,
            "-M",
            "bash",
            "-WWW",
            "-n",
            "-c",
            source,
        ],
    )

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
with open(log_path, "r", encoding="utf-8") as log:
    log_text = log.read()
os.unlink(log_path)
text = re.sub(r"\x1b\[[0-9;:]*m", "", output.decode(errors="replace")).replace("\r", "")
lines = text.split("\n")
content_index = next((index for index, line in enumerate(lines) if "$UNSET" in line), -1)
content = lines[content_index] if content_index >= 0 else ""
caret = next((line for line in lines[content_index + 1 :] if "^" in line), "")
highlight_byte_match = re.search(
    r"diagnostic highlighting consumed (\d+) source bytes", log_text
)
highlight_bytes = int(highlight_byte_match.group(1)) if highlight_byte_match else 0
lexical_byte_match = re.search(
    r"diagnostic lexical replay consumed (\d+) source bytes", log_text
)
lexical_bytes = int(lexical_byte_match.group(1)) if lexical_byte_match else 0
content_width = len(content) + content.count(wide_character)

within_width = bool(content) and content_width <= column_count
has_both_ellipses = "|  ..." in content and content.endswith("...")
caret_aligned = (
    "$UNSET" in content
    and "^" in caret
    and content.index("$")
    + content[: content.index("$")].count(wide_character)
    == caret.index("^")
)
printed_palette_is_applied = all(
    token in output
    for token in (
        b"\x1b[1;35mif\x1b[0m",
        b"\x1b[1;35m&&\x1b[0m",
        b"\x1b[1;35m>\x1b[0m",
        b"\x1b[96m$SECOND_UNSET\x1b[0m",
        b"\x1b[34mdiagnostic_missing_command\x1b[0m",
        b"\x1b[34mech\x1b[0m",
    )
)
warning_header_is_bold = bool(
    re.search(
        rb"\x1b\[1m\d+:\d+:\x1b\[0m \x1b\[1;33mwarning\x1b\[0m: "
        rb"\x1b\[1mA test reads an unquoted variable\.\x1b\[0m",
        output,
    )
)
caret_annotation_is_yellow = bool(
    re.search(rb"\x1b\[33m\^~*\x1b\[0m", output)
)
highlight_scan_is_bounded = 0 < highlight_bytes <= len(source_line.encode()) * 2 + 16
lexical_scan_is_bounded = 0 < lexical_bytes <= len(source.encode()) + 16
passed = (
    child_exited_cleanly
    and within_width
    and has_both_ellipses
    and caret_aligned
    and printed_palette_is_applied
    and warning_header_is_bold
    and caret_annotation_is_yellow
    and highlight_scan_is_bounded
    and lexical_scan_is_bounded
)

print("CHILD_EXITED_CLEANLY:", child_exited_cleanly)
print("WITHIN_WIDTH:", within_width)
print("BOTH_ELLIPSES:", has_both_ellipses)
print("CARET_ALIGNED:", caret_aligned)
print("PRINTED_PALETTE_IS_APPLIED:", printed_palette_is_applied)
print("WARNING_HEADER_IS_BOLD:", warning_header_is_bold)
print("CARET_ANNOTATION_IS_YELLOW:", caret_annotation_is_yellow)
print("HIGHLIGHT_SCAN_BOUNDED:", highlight_scan_is_bounded)
print("LEXICAL_SCAN_BOUNDED:", lexical_scan_is_bounded)
print("RESULT:", "PASS" if passed else "FAIL")
sys.exit(0 if passed else 1)
