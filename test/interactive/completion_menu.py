#!/usr/bin/env python3
"""A second tab opens the selectable candidate menu under the prompt.

The menu is drawn by the vendored editor in the rows below the input block, so
every check reads the raw transcript of a pty session. The interactive selector
is chosen with --tab-selector, since the plain listing is what a session gets
without it.
"""

import fcntl
import os
import pty
import select
import signal
import struct
import sys
import tempfile
import termios
import time

# Each scenario runs from a candidate tree. The binary is resolved before any
# directory change.
binary = os.path.abspath(sys.argv[1])

SELECTED_SGR = b"\x1b[7m"
GHOST_SGR = b"\x1b[90m"
HIGHLIGHT_RESET = b"\x1b[0m"
CLEAR_BELOW = b"\x1b[0J"
NO_MATCHES = b"no matches, erase to widen the search"


def read_until_idle(master, timeout, required_output=None, required_count=1):
    output = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        readable, _, _ = select.select([master], [], [], 0.1)
        if master not in readable:
            if output and (
                required_output is None
                or output.count(required_output) >= required_count
            ):
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


def run_menu(
    directory,
    tree,
    typed,
    keys,
    rows=24,
    cols=120,
    resized_rows=None,
    resized_cols=None,
    keys_before_resize=(),
    environment=None,
    open_key=b"\t",
    opened_required=None,
    first_key_required=None,
    key_outputs=None,
    first_open_output=None,
    key_required_outputs=(),
    key_blocked_completions=(),
    key_early_outputs=None,
):
    """Type the words, press the opening key twice, send the keys, and submit.

    The transcript is split at the second opening key. A check can tell what the
    menu drew from what the accepted line printed.
    """
    pid, master = pty.fork()
    if pid == 0:
        fcntl.ioctl(
            1, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0)
        )
        os.environ["TERM"] = "xterm-256color"
        os.environ["HOME"] = directory
        os.environ["KOSH_HISTORY_FILE"] = os.path.join(directory, "history")
        os.environ.pop("NO_COLOR", None)
        for name, value in (environment or {}).items():
            os.environ[name] = value
        os.chdir(os.path.join(directory, tree))
        os.execv(
            binary,
            [binary, "--norc", "--no-diagnostics", "--tab-selector",
             "interactive"],
        )

    read_until_idle(master, 3)
    os.write(master, typed.encode())
    read_until_idle(master, 1)

    # The first press only inserts the common prefix. The menu belongs to the
    # second one.
    os.write(master, open_key)
    first_opened = read_until_idle(master, 2, CLEAR_BELOW, 2)
    if first_open_output is not None:
        first_open_output.append(first_opened)

    os.write(master, open_key)
    menu = read_until_idle(
        master,
        2,
        SELECTED_SGR if opened_required is None else opened_required,
    )
    resized_menu = b""

    for key in keys_before_resize:
        os.write(master, key)
        menu += read_until_idle(master, 1)

    if resized_rows is not None or resized_cols is not None:
        new_rows = rows if resized_rows is None else resized_rows
        new_cols = cols if resized_cols is None else resized_cols
        fcntl.ioctl(
            master,
            termios.TIOCSWINSZ,
            struct.pack("HHHH", new_rows, new_cols, 0, 0),
        )
        resized_menu = read_until_idle(master, 2)

    for index, key in enumerate(keys):
        os.write(master, key)
        if index < len(key_blocked_completions):
            blocked_completion = key_blocked_completions[index]
        else:
            blocked_completion = None
        if blocked_completion is not None:
            started_path, release_path = blocked_completion
            deadline = time.monotonic() + 2
            while not os.path.exists(started_path) and time.monotonic() < deadline:
                time.sleep(0.01)
            early_output = read_until_idle(master, 2, b"loading...")
            if key_early_outputs is not None:
                key_early_outputs.append(early_output)
            open(release_path, "w").close()
            menu += early_output
        required = (
            key_required_outputs[index]
            if index < len(key_required_outputs)
            else first_key_required if index == 0 else None
        )
        key_output = read_until_idle(master, 1, required)
        menu += key_output
        if key_outputs is not None:
            key_outputs.append(key_output)

    os.write(master, b"\n")
    tail = read_until_idle(master, 2, b"MARKER-END")
    os.write(master, b"printf 'MARKER-END\\n'\nexit\n")
    tail += read_until_idle(master, 2)
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

    return menu, resized_menu, tail


def main():
    with tempfile.TemporaryDirectory() as directory:
        tree = os.path.join(directory, "tree")
        os.mkdir(tree)
        for name in ("alpha-one", "alpha-two", "alpha-three"):
            open(os.path.join(tree, name), "w").close()

        tall = os.path.join(directory, "tall")
        os.mkdir(tall)
        for index in range(1, 9):
            open(os.path.join(tall, "menu-%d" % index), "w").close()

        deep = os.path.join(directory, "deep")
        os.mkdir(deep)
        for name, entries in (
            ("deep-one", ("inner-alpha", "inner-beta")),
            ("deep-two", ("inner-gamma",)),
        ):
            os.mkdir(os.path.join(deep, name))
            for entry in entries:
                open(os.path.join(deep, name, entry), "w").close()

        wide = os.path.join(directory, "wide")
        os.mkdir(wide)
        long_candidate_prefix = "Blackmagic Design-DaVinci Resolve-"
        for suffix in ("Fusion", "Render"):
            open(os.path.join(wide, long_candidate_prefix + suffix), "w").close()

        semantic = os.path.join(directory, "semantic")
        os.mkdir(semantic)
        for name in ("--stale-one", "--stale-two"):
            open(os.path.join(semantic, name), "w").close()
        fake_bin = os.path.join(directory, "bin")
        os.mkdir(fake_bin)
        tailscale = os.path.join(fake_bin, "tailscale")
        with open(tailscale, "w") as fake:
            fake.write(
                "#!/bin/sh\n"
                "if [ \"$1\" = status ]; then\n"
                "  if [ -n \"${KOSH_TEST_COMPLETION_STARTED-}\" ]; then\n"
                "    : > \"$KOSH_TEST_COMPLETION_STARTED\"\n"
                "    while [ ! -e \"$KOSH_TEST_COMPLETION_RELEASE\" ]; do\n"
                "      sleep 0.01\n"
                "    done\n"
                "  fi\n"
                "  printf 'OPTIONS\\n  --json  Print JSON\\n"
                "  --peers  Print peers\\n'\n"
                "else\n"
                "  printf 'SUBCOMMANDS\\n  status  Show status\\n"
                "  stop  Stop Tailscale\\n'\n"
                "fi\n"
            )
        os.chmod(tailscale, 0o755)

        large = os.path.join(directory, "large")
        os.mkdir(large)
        for prefix in ("a", "b"):
            for index in range(384):
                open(
                    os.path.join(large, "bulk-%s-%03d" % (prefix, index)),
                    "w",
                ).close()

        typed = "printf '<%s>\\n' alpha"
        tall_typed = "printf '<%s>\\n' menu"
        deep_typed = "printf '<%s>\\n' deep"
        wide_typed = "printf '<%s>\\n' Blackmagic"

        first_open_output = []
        opened, _, _ = run_menu(
            directory,
            "tree",
            typed,
            [],
            opened_required=GHOST_SGR + b"one" + HIGHLIGHT_RESET,
            first_open_output=first_open_output,
        )
        initial_loading_position = (
            first_open_output[0].find(b"loading...")
            if len(first_open_output) == 1
            else -1
        )
        initial_completion_shows_loading_row = (
            initial_loading_position >= 0
            and first_open_output[0].find(
                CLEAR_BELOW, initial_loading_position + len(b"loading...")
            )
            > initial_loading_position
        )
        menu_lists_every_candidate = (
            b"alpha-one" in opened
            and b"alpha-two" in opened
            and b"alpha-three" in opened
        )
        menu_opens_with_first_selection = SELECTED_SGR in opened
        # The first row names the source and lists the keys it answers in the
        # dim of every other secondary text.
        help_row_names_the_source = (
            b"  "
            + GHOST_SGR
            + b"selecting completions"
            + HIGHLIGHT_RESET
            + GHOST_SGR
            + b", enter to run"
            in opened
        )
        selected_start = opened.find(SELECTED_SGR)
        selected_end = opened.find(HIGHLIGHT_RESET, selected_start)
        selected_text = opened[
            selected_start + len(SELECTED_SGR):selected_end
        ]
        # The candidates carry no description. The row ends right after the
        # name, and the highlight does not reach the width of the longest
        # entry.
        expected_selected_text = b" alpha-one "
        selected_highlight_ends_after_entry = (
            selected_start >= 0
            and selected_end >= 0
            and selected_text == expected_selected_text
        )

        # The first tab inserted the common prefix alpha-. The preview of the
        # first row is the rest of alpha-one, drawn dimmed on the line the menu
        # opened on.
        preview_shows_the_selected_candidate = (
            GHOST_SGR + b"one" + HIGHLIGHT_RESET in opened
        )

        moved, _, submitted = run_menu(
            directory,
            "tree",
            typed,
            [b"\x1b[B", b"\n"],
            first_key_required=GHOST_SGR + b"three" + HIGHLIGHT_RESET,
        )
        a_movement_key_highlights_a_row = SELECTED_SGR in moved
        # The down arrow moves to alpha-three, since shift tab reaches
        # alpha-two as the last row, and the preview follows the highlight.
        preview_follows_the_highlight = (
            GHOST_SGR + b"three" + HIGHLIGHT_RESET in moved
        )
        # Enter leaves the highlighted row behind and runs the line the first
        # tab grew to the common prefix. The key loop collects the output of
        # that run, since the trailing newline of the driver comes later.
        enter_submits_the_line = b"<alpha->" in moved

        _, _, tab_accepted = run_menu(directory, "tree", typed, [b"\t"])
        tab_accepts_current_selection = b"<alpha-one>" in tab_accepted

        shifted, _, shifted_accepted = run_menu(
            directory, "tree", typed, [b"\x1b[Z", b"\t"]
        )
        shift_tab_wraps_backward = (
            SELECTED_SGR in shifted and b"<alpha-two>" in shifted_accepted
        )

        _, _, dismissed = run_menu(directory, "tree", typed, [b"\x1b"])
        escape_leaves_the_line_alone = b"<alpha->" in dismissed

        _, _, typed_through = run_menu(directory, "tree", typed, [b"x"])
        an_ordinary_key_reaches_the_line = b"<alpha-x>" in typed_through

        # Typing narrows the list. The Tab that follows is answered by the
        # menu. A closed menu would submit alpha-t.
        _, _, filtered = run_menu(directory, "tree", typed, [b"t", b"\t"])
        typing_narrows_the_list = b"<alpha-three>" in filtered

        # The narrowing reaches a candidate the typed bytes neither open nor
        # spell out. The bytes of alpha-hr appear in order inside alpha-three.
        # A list narrowed by prefix alone would have emptied here.
        _, _, fuzzy = run_menu(directory, "tree", typed, [b"h", b"r", b"\t"])
        a_fuzzy_search_reaches_a_candidate = b"<alpha-three>" in fuzzy

        # Backspace widens the list back to every candidate. The first row is
        # alpha-one again. A closed menu would submit alpha- on its own.
        _, _, widened = run_menu(
            directory, "tree", typed, [b"t", b"\x7f", b"\t"]
        )
        backspace_widens_the_list = b"<alpha-one>" in widened

        # The first Ctrl-W erases the common-prefix hyphen and the second
        # erases alpha. Tab still belongs to the open menu and accepts its
        # first row. A closed menu would only restore the alpha- prefix.
        word_erase_outputs = []
        _, _, word_erased = run_menu(
            directory,
            "tree",
            typed,
            [b"\x17", b"\x17", b"\t"],
            key_outputs=word_erase_outputs,
            key_required_outputs=(SELECTED_SGR, SELECTED_SGR),
        )
        whole_word_backspace_keeps_menu_open = (
            len(word_erase_outputs) == 3
            and SELECTED_SGR in word_erase_outputs[1]
            and b"<alpha-one>" in word_erased
        )

        empty_word_outputs = []
        run_menu(
            directory,
            "tree",
            typed,
            [b"\x7f"] * 7 + [b"\x07"],
            key_outputs=empty_word_outputs,
            key_required_outputs=(SELECTED_SGR,) * 6 + (NO_MATCHES,),
        )
        exact_empty_word_keeps_menu_open = (
            len(empty_word_outputs) == 8
            and SELECTED_SGR in empty_word_outputs[5]
        )
        erase_across_word_boundary_keeps_menu_open = (
            len(empty_word_outputs) == 8
            and NO_MATCHES in empty_word_outputs[6]
        )

        completion_started = os.path.join(directory, "completion-started")
        completion_release = os.path.join(directory, "completion-release")
        semantic_early_outputs = []
        semantic_menu, _, _ = run_menu(
            directory,
            "semantic",
            "tailscale s",
            [b"atus --"],
            environment={
                "PATH": fake_bin + os.pathsep + os.environ.get("PATH", ""),
                "KOSH_TEST_COMPLETION_STARTED": completion_started,
                "KOSH_TEST_COMPLETION_RELEASE": completion_release,
            },
            first_key_required=b"--json",
            key_blocked_completions=((completion_started, completion_release),),
            key_early_outputs=semantic_early_outputs,
        )
        each_new_word_regathers_completions = (
            b"--json" in semantic_menu and b"--peers" in semantic_menu
        )
        loading_position = semantic_menu.find(b"loading...")
        final_candidate_position = semantic_menu.find(b"--json")
        replacement_position = semantic_menu.find(
            CLEAR_BELOW, loading_position + len(b"loading...")
        )
        regather_shows_loading_row = (
            loading_position >= 0
            and replacement_position > loading_position
            and final_candidate_position > replacement_position
        )
        loading_is_visible_while_gathering = (
            len(semantic_early_outputs) == 1
            and b"loading..." in semantic_early_outputs[0]
            and os.path.exists(completion_started)
        )

        _, _, large_tail = run_menu(
            directory,
            "large",
            "printf '<%s>\\n' bulk",
            [b"a", b"\x7f", b"z", b"z", b"\x07"],
            environment={"KOSH_TEST_EDITOR_STATS": "1"},
            key_required_outputs=(
                SELECTED_SGR,
                SELECTED_SGR,
                b"no matches",
                b"no matches",
            ),
        )
        metrics_start = large_tail.find(b"editor-refresh ")
        metrics_end = large_tail.find(b"\n", metrics_start)
        metrics_line = (
            large_tail[metrics_start:metrics_end]
            if metrics_start >= 0 and metrics_end >= 0
            else b""
        )
        metrics = dict(
            field.split(b"=", 1)
            for field in metrics_line.split()[1:]
            if b"=" in field
        )
        large_file_menu_reuses_warm_index = (
            metrics.get(b"stats") == b"0"
            and metrics.get(b"reads") == b"0"
            and metrics.get(b"sorts") == b"0"
        )
        large_file_menu_filters_without_regather = (
            metrics.get(b"listings") == b"3"
        )

        # A search that matches nothing keeps the menu open on the row that says
        # so, and the erase that follows brings the list back for the Tab.
        emptied, _, recovered = run_menu(
            directory, "tree", typed, [b"z", b"z", b"\x7f", b"\x7f", b"\t"]
        )
        an_empty_search_keeps_the_menu_open = b"no matches" in emptied
        an_erase_recovers_the_list = b"<alpha-one>" in recovered

        # Accepting a directory walks into it. The second Tab answers the menu
        # the directory opened. A closed menu would submit deep-one/.
        _, _, descended = run_menu(
            directory, "deep", deep_typed, [b"\t", b"\t"]
        )
        a_directory_opens_its_own_menu = (
            b"<deep-one/inner-alpha>" in descended
        )

        # A slash typed into an open menu names a directory. The rows become the
        # entries of that directory, and the Tab that follows accepts one of
        # them. A menu that only narrowed its parent listing would submit
        # deep-one/ on its own.
        walked, _, _ = run_menu(
            directory, "deep", deep_typed, [b"o", b"n", b"e", b"/"]
        )
        a_slash_walks_into_the_directory = b"inner-alpha" in walked

        _, _, walked_accepted = run_menu(
            directory, "deep", deep_typed,
            [b"o", b"n", b"e", b"/", b"\t"]
        )
        a_walked_menu_accepts_an_entry = (
            b"<deep-one/inner-alpha>" in walked_accepted
        )

        # Escape puts back the line the menu opened on. The narrowing key is
        # undone. A menu that cancelled in place would submit alpha-t.
        _, _, cancelled = run_menu(directory, "tree", typed, [b"t", b"\x1b"])
        escape_restores_the_opening_line = b"<alpha->" in cancelled

        # Control G cancels the same way Escape does, here after the menu walked
        # into a directory. A menu that cancelled in place would submit
        # deep-one/.
        _, _, aborted = run_menu(
            directory, "deep", deep_typed, [b"\t", b"\x07"]
        )
        control_g_restores_the_opening_line = b"<deep->" in aborted

        # Eight candidates in an eight row terminal cannot all be shown. The
        # menu bounds its rows and names the part it drew.
        bounded, _, _ = run_menu(directory, "tall", tall_typed, [], rows=8)
        a_long_list_is_bounded = b"showing 1-" in bounded and b" of 8" in bounded
        the_first_candidate_is_visible = b"menu-1" in bounded

        wide_menu, _, _ = run_menu(directory, "wide", wide_typed, [])
        escaped_prefix = long_candidate_prefix.replace(" ", "\\ ").encode()
        long_candidate_uses_available_width = (
            escaped_prefix + b"Fusion" in wide_menu
        )

        _, expanded, _ = run_menu(
            directory, "tall", tall_typed, [], rows=8, resized_rows=14
        )
        resize_expands_the_visible_window = b"menu-8" in expanded

        _, contracted, _ = run_menu(
            directory, "tall", tall_typed, [], rows=14, resized_rows=8
        )
        resize_contracts_the_visible_window = (
            b"menu-8" not in contracted
            and b"showing 1-" in contracted
            and b" of 8" in contracted
        )

        _, selected_after_resize, selected_tail = run_menu(
            directory,
            "tall",
            tall_typed,
            [b"\t"],
            rows=14,
            resized_rows=8,
            keys_before_resize=(b"\x1b[B",) * 7,
        )
        resize_keeps_the_selection_visible = (
            SELECTED_SGR in selected_after_resize
            and b"menu-8" in selected_after_resize
        )
        resized_selection_is_accepted = b"<menu-8>" in selected_tail

        # A narrow terminal rewraps the typed line over the whole block. The
        # help row, the candidates, and the count row have no room left under
        # it. A menu that drew them anyway would scroll the prompt off the top.
        # Every row the repaint advances over carries its own newline, and the
        # block and the menu together stay inside the terminal.
        wrapped_typed = "printf '<%s>\\n'" + " " * 40 + "menu"
        _, rewrapped, rewrapped_tail = run_menu(
            directory, "tall", wrapped_typed, [], rows=8, resized_cols=12
        )
        a_rewrap_keeps_the_menu_inside_the_terminal = (
            b"printf" in rewrapped and rewrapped.count(b"\r\n") + 1 <= 8
        )
        a_rewrap_keeps_the_prompt_usable = b"MARKER-END" in rewrapped_tail

        # A refused color leaves the editor drawing plain text. The reversed
        # band of the selected row survives, since reverse video carries no
        # color of its own.
        plain, _, plain_accepted = run_menu(
            directory,
            "tree",
            typed,
            [b"\t"],
            environment={"NO_COLOR": "1"},
        )
        no_color_drops_the_help_row_color = (
            b"  selecting completions, enter to run" in plain
            and GHOST_SGR not in plain
        )
        no_color_keeps_the_selection_band = SELECTED_SGR in plain
        no_color_keeps_the_menu_usable = b"<alpha-one>" in plain_accepted

        # Control space carries the null byte a terminal sends, and the editor
        # answers it the way it answers Tab.
        by_control_space, _, control_space_accepted = run_menu(
            directory, "tree", typed, [b"\t"], open_key=b"\x00"
        )
        control_space_opens_the_menu = SELECTED_SGR in by_control_space
        control_space_accepts_a_candidate = (
            b"<alpha-one>" in control_space_accepted
        )

        prompt_stays_usable = b"MARKER-END" in submitted

        results = {
            "MENU_LISTS_EVERY_CANDIDATE": menu_lists_every_candidate,
            "MENU_OPENS_WITH_FIRST_SELECTION": menu_opens_with_first_selection,
            "INITIAL_COMPLETION_SHOWS_LOADING_ROW": (
                initial_completion_shows_loading_row
            ),
            "HELP_ROW_NAMES_THE_SOURCE": help_row_names_the_source,
            "SELECTED_HIGHLIGHT_ENDS_AFTER_ENTRY": (
                selected_highlight_ends_after_entry
            ),
            "PREVIEW_SHOWS_THE_SELECTED_CANDIDATE": (
                preview_shows_the_selected_candidate
            ),
            "PREVIEW_FOLLOWS_THE_HIGHLIGHT": preview_follows_the_highlight,
            "A_MOVEMENT_KEY_HIGHLIGHTS_A_ROW": a_movement_key_highlights_a_row,
            "ENTER_SUBMITS_THE_LINE": enter_submits_the_line,
            "TAB_ACCEPTS_CURRENT_SELECTION": tab_accepts_current_selection,
            "SHIFT_TAB_WRAPS_BACKWARD": shift_tab_wraps_backward,
            "ESCAPE_LEAVES_THE_LINE_ALONE": escape_leaves_the_line_alone,
            "AN_ORDINARY_KEY_REACHES_THE_LINE": (
                an_ordinary_key_reaches_the_line
            ),
            "TYPING_NARROWS_THE_LIST": typing_narrows_the_list,
            "A_FUZZY_SEARCH_REACHES_A_CANDIDATE": (
                a_fuzzy_search_reaches_a_candidate
            ),
            "BACKSPACE_WIDENS_THE_LIST": backspace_widens_the_list,
            "WHOLE_WORD_BACKSPACE_KEEPS_MENU_OPEN": (
                whole_word_backspace_keeps_menu_open
            ),
            "EXACT_EMPTY_WORD_KEEPS_MENU_OPEN": (
                exact_empty_word_keeps_menu_open
            ),
            "ERASE_ACROSS_WORD_BOUNDARY_KEEPS_MENU_OPEN": (
                erase_across_word_boundary_keeps_menu_open
            ),
            "EACH_NEW_WORD_REGATHERS_COMPLETIONS": (
                each_new_word_regathers_completions
            ),
            "REGATHER_SHOWS_LOADING_ROW": regather_shows_loading_row,
            "LOADING_IS_VISIBLE_WHILE_GATHERING": (
                loading_is_visible_while_gathering
            ),
            "LARGE_FILE_MENU_REUSES_WARM_INDEX": (
                large_file_menu_reuses_warm_index
            ),
            "LARGE_FILE_MENU_FILTERS_WITHOUT_REGATHER": (
                large_file_menu_filters_without_regather
            ),
            "AN_EMPTY_SEARCH_KEEPS_THE_MENU_OPEN": (
                an_empty_search_keeps_the_menu_open
            ),
            "AN_ERASE_RECOVERS_THE_LIST": an_erase_recovers_the_list,
            "A_DIRECTORY_OPENS_ITS_OWN_MENU": a_directory_opens_its_own_menu,
            "A_SLASH_WALKS_INTO_THE_DIRECTORY": (
                a_slash_walks_into_the_directory
            ),
            "A_WALKED_MENU_ACCEPTS_AN_ENTRY": a_walked_menu_accepts_an_entry,
            "ESCAPE_RESTORES_THE_OPENING_LINE": (
                escape_restores_the_opening_line
            ),
            "CONTROL_G_RESTORES_THE_OPENING_LINE": (
                control_g_restores_the_opening_line
            ),
            "A_LONG_LIST_IS_BOUNDED": a_long_list_is_bounded,
            "THE_FIRST_CANDIDATE_IS_VISIBLE": the_first_candidate_is_visible,
            "LONG_CANDIDATE_USES_AVAILABLE_WIDTH": (
                long_candidate_uses_available_width
            ),
            "RESIZE_EXPANDS_THE_VISIBLE_WINDOW": (
                resize_expands_the_visible_window
            ),
            "RESIZE_CONTRACTS_THE_VISIBLE_WINDOW": (
                resize_contracts_the_visible_window
            ),
            "RESIZE_KEEPS_THE_SELECTION_VISIBLE": (
                resize_keeps_the_selection_visible
            ),
            "RESIZED_SELECTION_IS_ACCEPTED": resized_selection_is_accepted,
            "A_REWRAP_KEEPS_THE_MENU_INSIDE_THE_TERMINAL": (
                a_rewrap_keeps_the_menu_inside_the_terminal
            ),
            "A_REWRAP_KEEPS_THE_PROMPT_USABLE": (
                a_rewrap_keeps_the_prompt_usable
            ),
            "NO_COLOR_DROPS_THE_HELP_ROW_COLOR": (
                no_color_drops_the_help_row_color
            ),
            "NO_COLOR_KEEPS_THE_SELECTION_BAND": (
                no_color_keeps_the_selection_band
            ),
            "NO_COLOR_KEEPS_THE_MENU_USABLE": no_color_keeps_the_menu_usable,
            "CONTROL_SPACE_OPENS_THE_MENU": control_space_opens_the_menu,
            "CONTROL_SPACE_ACCEPTS_A_CANDIDATE": (
                control_space_accepts_a_candidate
            ),
            "PROMPT_STAYS_USABLE": prompt_stays_usable,
        }

    passed = all(results.values())
    for name, value in results.items():
        print(f"{name}: {value}")
    print("COMPLETION_MENU:", passed)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
