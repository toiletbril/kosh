#!/usr/bin/env python3
"""Ctrl-R opens the incremental history search under the plain selector.

The search draws the match, the query, and the hint in the rows below the
prompt, so every check reads the raw transcript of a pty session. No
--tab-selector is passed, since the incremental search is what a session gets
without it. The keys it answers are the keys the interactive menu answers.
"""

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

binary = os.path.abspath(sys.argv[1])

# The word each command prints is an operand. A drawn match never carries the
# angle bracketed output that proves the entry ran.
SEEDED_COMMANDS = (
    "printf '<%s>\\n' one",
    "printf '<%s>\\n' two",
    "printf '<%s>\\n' three",
)

# A Tab that reached completion would take this name over the last word of the
# accepted match.
COMPLETION_BAIT = "threexyz"

# Every run owns a history file of its own. A shared one would carry the
# entries of the preceding run into the next search.
run_count = 0


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


def run_history_search(
    directory,
    keys,
    rows=24,
    add_peer=False,
    no_completion=False,
    recall_before_search=False,
    post_search_keys=(),
    tail_source=None,
):
    """Seed the history, press ctrl-R, send the keys, then submit the line.

    The transcript is split at ctrl-R. A check can tell what the search drew
    from what the accepted line printed.
    """
    global run_count
    run_count += 1
    history_file = os.path.join(directory, f"history-{run_count}")

    pid, master = pty.fork()
    if pid == 0:
        fcntl.ioctl(1, termios.TIOCSWINSZ, struct.pack("HHHH", rows, 120, 0, 0))
        os.environ["TERM"] = "xterm-256color"
        os.environ["HOME"] = directory
        os.environ["KOSH_HISTORY_FILE"] = history_file
        os.chdir(directory)
        arguments = [
            binary,
            "--norc",
            "--no-diagnostics",
            "--tab-selector",
            "plain",
        ]
        if no_completion:
            arguments.append("--no-completion")
        os.execv(binary, arguments)

    read_until_idle(master, 3)
    for command in SEEDED_COMMANDS:
        os.write(master, command.encode() + b"\n")
        read_until_idle(master, 2)

    if add_peer:
        peer_environment = os.environ.copy()
        peer_environment["HOME"] = directory
        peer_environment["KOSH_HISTORY_FILE"] = history_file
        subprocess.run(
            [
                binary,
                "--no-init-files",
                "-c",
                "history -s 'echo PEER-ONLY-HISTORY'",
            ],
            env=peer_environment,
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    if recall_before_search:
        os.write(master, b"\x1b[A")
        read_until_idle(master, 1)
        os.write(master, b"\x15")
        read_until_idle(master, 1)

    os.write(master, b"\x12")
    search = read_until_idle(master, 2)

    for key in keys:
        os.write(master, key)
        search += read_until_idle(master, 1)

    for key in post_search_keys:
        os.write(master, key)
        search += read_until_idle(master, 1)

    # Accepting a match only rewrites the line. The run needs a submit of its
    # own.
    os.write(master, b"\n")
    search += read_until_idle(master, 2)
    if tail_source is None:
        tail_source = "printf 'MARKER-%s\\n' END"
    os.write(master, tail_source.encode() + b"\nexit\n")
    tail = read_until_idle(master, 2)
    os.close(master)

    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        waited, _ = os.waitpid(pid, os.WNOHANG)
        if waited == pid:
            break
        time.sleep(0.02)
    else:
        os.kill(pid, signal.SIGKILL)
        os.waitpid(pid, 0)

    return search, tail


def main():
    with tempfile.TemporaryDirectory() as directory:
        with open(os.path.join(directory, COMPLETION_BAIT), "w") as bait:
            bait.write("")

        # The query reaches every seeded command, and the newest of them is
        # drawn first. Enter takes it without running it.
        accepted, marker = run_history_search(directory, [b"printf", b"\n"])
        enter_accepts_the_newest_match = b"<three>" in accepted

        # The hint names each key beside what it does.
        hint_names_the_keys = (
            b"to move" in accepted
            and b"to accept" in accepted
            and b"to cancel" in accepted
        )

        # The down arrow steps back to an older match, the direction ctrl-R
        # walks.
        moved, _ = run_history_search(
            directory, [b"printf", b"\x1b[B", b"\n"]
        )
        the_down_arrow_reaches_an_older_match = b"<two>" in moved

        # The up arrow steps forward again, the direction ctrl-F walks.
        returned, _ = run_history_search(
            directory, [b"printf", b"\x1b[B", b"\x1b[A", b"\n"]
        )
        the_up_arrow_returns_to_a_newer_match = b"<three>" in returned

        # Ctrl-R and ctrl-F keep the movement they always had.
        stepped, _ = run_history_search(
            directory, [b"printf", b"\x12", b"\x12", b"\x06", b"\n"]
        )
        the_control_keys_still_step = b"<two>" in stepped

        # Tab takes the match and is not passed on. A Tab that reached
        # completion would take the bait name over the last word.
        tabbed, _ = run_history_search(directory, [b"printf", b"\t"])
        tab_accepts_the_match = b"<three>" in tabbed
        tab_does_not_reach_completion = b"<" + COMPLETION_BAIT.encode() + b">" \
            not in tabbed

        # Escape puts back the line the search opened on. That line is empty
        # here. The command typed afterwards runs on its own.
        cancelled, _ = run_history_search(
            directory, [b"printf", b"\x1b", b"printf '<%s>\\n' kept", b"\n"]
        )
        escape_leaves_the_line_alone = b"<kept>" in cancelled

        # Ctrl-G cancels the same way.
        aborted, _ = run_history_search(
            directory, [b"printf", b"\x07", b"printf '<%s>\\n' kept", b"\n"]
        )
        control_g_leaves_the_line_alone = b"<kept>" in aborted

        # A query no entry matches leaves the line empty, and the search draws
        # nothing to accept.
        unmatched, _ = run_history_search(directory, [b"zzz", b"\n"])
        an_unmatched_query_accepts_nothing = b"<three>" not in unmatched

        private_history_path = os.path.join(directory, "private-history")
        peer_search, peer_marker = run_history_search(
            directory,
            [b"PEER-ONLY", b"\t"],
            add_peer=True,
            recall_before_search=True,
            post_search_keys=(b"\x1b[A", b"\x1b[B", b"\x15"),
            tail_source=(
                f"history > {private_history_path}; "
                "printf 'PEER-MARKER-%s\\n' END"
            ),
        )
        with open(private_history_path, encoding="utf-8") as private_history:
            private_listing = private_history.read()
        search_reads_peer_history = (
            b"echo PEER-ONLY-HISTORY" in peer_search
        )
        search_does_not_merge_peer_history = (
            "echo PEER-ONLY-HISTORY" not in private_listing
            and SEEDED_COMMANDS[-1] in private_listing
            and b"PEER-MARKER-END" in peer_marker
        )
        accepted_peer_returns_to_private_navigation = (
            SEEDED_COMMANDS[-1].encode() in peer_search
        )

        no_completion_search, _ = run_history_search(
            directory,
            [b"PEER-ONLY", b"\x1b"],
            add_peer=True,
            no_completion=True,
        )
        no_completion_still_reads_peer_history = (
            b"echo PEER-ONLY-HISTORY" in no_completion_search
        )

        prompt_stays_usable = b"MARKER-END" in marker

        results = {
            "ENTER_ACCEPTS_THE_NEWEST_MATCH": enter_accepts_the_newest_match,
            "HINT_NAMES_THE_KEYS": hint_names_the_keys,
            "THE_DOWN_ARROW_REACHES_AN_OLDER_MATCH": (
                the_down_arrow_reaches_an_older_match
            ),
            "THE_UP_ARROW_RETURNS_TO_A_NEWER_MATCH": (
                the_up_arrow_returns_to_a_newer_match
            ),
            "THE_CONTROL_KEYS_STILL_STEP": the_control_keys_still_step,
            "TAB_ACCEPTS_THE_MATCH": tab_accepts_the_match,
            "TAB_DOES_NOT_REACH_COMPLETION": tab_does_not_reach_completion,
            "ESCAPE_LEAVES_THE_LINE_ALONE": escape_leaves_the_line_alone,
            "CONTROL_G_LEAVES_THE_LINE_ALONE": control_g_leaves_the_line_alone,
            "AN_UNMATCHED_QUERY_ACCEPTS_NOTHING": (
                an_unmatched_query_accepts_nothing
            ),
            "SEARCH_READS_PEER_HISTORY": search_reads_peer_history,
            "SEARCH_DOES_NOT_MERGE_PEER_HISTORY": (
                search_does_not_merge_peer_history
            ),
            "ACCEPTED_PEER_RETURNS_TO_PRIVATE_NAVIGATION": (
                accepted_peer_returns_to_private_navigation
            ),
            "NO_COMPLETION_STILL_READS_PEER_HISTORY": (
                no_completion_still_reads_peer_history
            ),
            "PROMPT_STAYS_USABLE": prompt_stays_usable,
        }

    passed = all(results.values())
    for name, value in results.items():
        print(f"{name}: {value}")
    print("HISTORY_SEARCH:", passed)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
