ifeq ($(OS), Windows_NT)
SHELL := sh
TEST_ABSOLUTE_DIRECTORY_COMMAND := pwd -W
else
SHELL := /bin/bash
TEST_ABSOLUTE_DIRECTORY_COMMAND := pwd
endif

DIFF_FLAGS := -u
MODE ?= dbg
IS_NONDEBUG_BUILD := $(if $(filter rel prof cov cosmo,$(MODE)),1,0)
export IS_NONDEBUG_BUILD
ifeq ($(OS), Windows_NT)
TARGET ?= Windows_NT
else
TARGET ?= $(shell uname -s)
endif

ifeq ($(TARGET), Windows_NT)
TEST_JOBS := 1
export TEST_PATH_SEPARATOR := ;
export TEST_NULL_DEVICE := NUL
export TEST_PATH_ENVIRONMENT_NAME := Path
export MSYS2_ENV_CONV_EXCL := Path
export HOME := $(USERPROFILE)
else
export TEST_PATH_SEPARATOR := :
export TEST_NULL_DEVICE := /dev/null
export TEST_PATH_ENVIRONMENT_NAME := PATH
endif
TEST_JOBS ?= $(shell ../scripts/cpu-count.sh)
export TEST_HOST_PATH_SEPARATOR := :
export TEST_TEMP_DIRECTORY := .test-work
export TEST_MKTEMP_DIRECTORY := $(shell $(TEST_ABSOLUTE_DIRECTORY_COMMAND))/.test-work

MODE_SUFFIX := $(if $(filter rel,$(MODE)),,-$(MODE))
ifeq ($(MODE), cosmo)
BIN ?= ../kosh$(MODE_SUFFIX).com
else ifeq ($(TARGET), Windows_NT)
BIN ?= ../kosh$(MODE_SUFFIX).exe
DIFF_FLAGS += -w -a # CRLF on Windows SMH
else
BIN ?= ../kosh$(MODE_SUFFIX)
endif

# The test drivers run with the current directory set to test, so a BIN given
# relative to the repo root or as a bare name is resolved to an absolute path
# here. A path that exists as given wins, then the repo-root sibling, then a
# PATH lookup, so the target binary is found no matter where make was launched.
override BIN := $(shell \
	if [ -x "$(BIN)" ]; then \
		echo "$$(cd "$$(dirname "$(BIN)")" && $(TEST_ABSOLUTE_DIRECTORY_COMMAND))/$$(basename "$(BIN)")"; \
	elif [ -x "$(CURDIR)/../$(BIN)" ]; then \
		candidate="$(CURDIR)/../$(BIN)"; \
		echo "$$(cd "$$(dirname "$$candidate")" && $(TEST_ABSOLUTE_DIRECTORY_COMMAND))/$$(basename "$$candidate")"; \
	elif command -v "$(BIN)" >/dev/null 2>&1; then \
		found=$$(command -v "$(BIN)"); \
		echo "$$(cd "$$(dirname "$$found")" && $(TEST_ABSOLUTE_DIRECTORY_COMMAND))/$$(basename "$$found")"; \
	else echo "$(BIN)"; fi)
export BIN

BIN_FLAGS := -AER
FAILED_LIST := failed.diff
export BIN_FLAGS DIFF_FLAGS FAILED_LIST TEST_JOBS
export TEST_SHELL := $(SHELL)

export KOSH_HISTORY := $(CURDIR)/.test_history
export KOSH_DIRECTORY_HISTORY := $(CURDIR)/.test_directory_history
ifeq ($(TARGET), Windows_NT)
export TEST_SYSTEM_PATH := .test-work/system-path
export TEST_UNAME_DIRECTORY := .test-work/system-path
else
export TEST_SYSTEM_PATH := $(shell \
	shell_path=$$(command -v sh); printf '%s' "$${shell_path%/*}")
export TEST_UNAME_DIRECTORY := $(shell \
	uname_path=$$(command -v uname); printf '%s' "$${uname_path%/*}")
endif
export PATH := $(CURDIR)/bin$(TEST_HOST_PATH_SEPARATOR)$(PATH)

%: expected/%.out
	@BIN='$(BIN)' BIN_FLAGS='$(BIN_FLAGS)' DIFF_FLAGS='$(DIFF_FLAGS)' \
		FAILED_LIST='$(FAILED_LIST)' $(SHELL) run-kosh-test.sh $*

clean:
	@rm -f $(FAILED_LIST) $(KOSH_HISTORY) $(KOSH_DIRECTORY_HISTORY)
	@test -n "$(CURDIR)/.test-work" && rm -rf "$(CURDIR)/.test-work"

test: $(BIN)
	@$(SHELL) run-test-suite.sh all

kosh_tests: $(BIN)
	@$(SHELL) run-test-suite.sh kosh

cli_%: expected/%.out
	@BIN='$(BIN)' DIFF_FLAGS='$(DIFF_FLAGS)' FAILED_LIST='$(FAILED_LIST)' \
		$(SHELL) run-cli-test.sh '$(SHELL)' cli/$*.sh

cli_tests: $(BIN)
	@$(SHELL) run-test-suite.sh cli

build_%: expected/%.out
	@$(SHELL) run-build-test.sh '$(SHELL)' build/$*.sh

build_tests: $(BIN)
	@$(SHELL) run-test-suite.sh build

completion_%: expected/%.out
	@$(SHELL) run-completion-test.sh '$(SHELL)' completion/$*.sh

ifeq ($(TARGET), Windows_NT)
WINDOWS_SKIPPED_TEST_NAMES := \
	ast_process_substitution \
	ast_redirection_varieties \
	heredoc_fd_three \
	lex_process_substitution \
	mood_conditional \
	redirect_csh_both_streams \
	redirect_dynamic_fd \
	redirect_high_fd \
	redirections \
	rerr_redirect_to_directory \
	umask_modes

$(WINDOWS_SKIPPED_TEST_NAMES):
	@printf "	%-64s skipped, unsupported Windows backend feature\n" "$@.kosh"

cli_assimilate:
	@printf "	%-64s skipped, remote transaction requires POSIX\n" "cli/assimilate.sh"

cli_bash_xtracefd:
	@printf "	%-64s skipped, numbered descriptors are unsupported on Windows\n" \
		"cli/bash_xtracefd.sh"

WINDOWS_SKIPPED_CLI_NAMES := \
	cli_command_substitution_interrupt \
	cli_command_substitution_strategy \
	cli_ctrl_c_interrupts_bash_loop \
	cli_deep_nesting_guard \
	cli_diagnostic_levels \
	cli_disown_running_only \
	cli_exec_behavior \
	cli_fg_terminal_handoff \
	cli_job_state_running_then_stopped \
	cli_mkdir_mode_umask \
	cli_path_cache_behavior \
	cli_regex_error_caret \
	cli_signal_noninteractive_term \
	cli_set_option_state \
	cli_spawn_failure_location \
	cli_stopped_job_survives_poll \
	cli_suspend_behavior \
	cli_wait_on_stopped_job

$(WINDOWS_SKIPPED_CLI_NAMES):
	@printf "	%-64s skipped, unsupported Windows backend feature\n" \
		"cli/$(patsubst cli_%,%,$@).sh"

build_cpu_count:
	@printf "	%-64s skipped, unsupported Windows build probe\n" \
		"build/cpu_count.sh"

WINDOWS_SKIPPED_COMPLETION_NAMES := \
	completion_complete_git_grouped_subcommands \
	completion_complete_go_subcommands \
	completion_complete_help_subcommands \
	completion_editor_append_hot_path \
	completion_help_options_trusted_dir_only \
	completion_help_subcommand_second_level \
	completion_help_subcommand_third_level \
	completion_help_subcommands_no_colon \
	completion_manpage_command_completion

$(WINDOWS_SKIPPED_COMPLETION_NAMES):
	@printf "	%-64s skipped, needs a POSIX helper executable\n" "$@"

WINDOWS_UNREPRESENTABLE_COMPLETION_NAMES := \
	completion_completion_glob_quoting \
	completion_path_quoting

$(WINDOWS_UNREPRESENTABLE_COMPLETION_NAMES):
	@printf "	%-64s skipped, golden test filenames are unsupported on Windows\n" \
		"$(patsubst completion_%,completion/%,$@).sh"
endif

completion_tests: $(BIN)
	@$(SHELL) run-test-suite.sh completion


highlight_%: expected/%.out
	@BIN='$(BIN)' DIFF_FLAGS='$(DIFF_FLAGS)' FAILED_LIST='$(FAILED_LIST)' \
		$(SHELL) run-highlight-test.sh '$(SHELL)' highlight/$*.sh

highlight_tests: $(BIN)
	@$(SHELL) run-test-suite.sh highlight

SKIPPED_TEST_NAMES := $(WINDOWS_SKIPPED_TEST_NAMES)
SKIPPED_CLI_INPUT := $(patsubst cli_%,cli/%.sh,$(WINDOWS_SKIPPED_CLI_NAMES))
SKIPPED_BUILD_INPUT :=
SKIPPED_COMPLETION_INPUT := \
	$(patsubst completion_%,completion/%.sh,$(WINDOWS_SKIPPED_COMPLETION_NAMES))
UNREPRESENTABLE_COMPLETION_INPUT := \
	$(patsubst completion_%,completion/%.sh,$(WINDOWS_UNREPRESENTABLE_COMPLETION_NAMES))
ifeq ($(TARGET), Windows_NT)
SKIP_CLI_ASSIMILATE := 1
SKIP_CLI_BASH_XTRACEFD := 1
SKIPPED_BUILD_INPUT := build/cpu_count.sh
endif

export SKIPPED_TEST_NAMES SKIPPED_CLI_INPUT SKIPPED_BUILD_INPUT
export SKIPPED_COMPLETION_INPUT UNREPRESENTABLE_COMPLETION_INPUT
export SKIP_CLI_ASSIMILATE SKIP_CLI_BASH_XTRACEFD

diagnostic_tty_test: $(BIN)
	@$(SHELL) run-test-suite.sh interactive


REFILL ?=

refill:
	@REFILL='$(REFILL)' $(SHELL) run-refill.sh '$(SHELL)'


ZSH ?= /bin/zsh
ASH ?= /bin/busybox
YASH ?= /bin/yash
PYTHON ?= python3
BASHP ?= /bin/bash
DASH ?= /bin/dash

export BASHP DASH

compatdiff: $(BIN)
	@$(SHELL) run-test-suite.sh compat

dashdiff bashdiff mimicrydiff: compatdiff


BENCH := bench/configure.sh
BENCH_BASH := bench/configure.bash
BENCH_KOSH := bench/configure.kosh
PRIMES := bench/primes.bash
PRIMES_PY := bench/primes.py
PRIMES_LIMIT ?= 100000
SCALE ?= 100
BENCH_WARMUP_COUNT ?= 0
BENCH_SAMPLE_COUNT ?= 1
DASH_SPEEDUP_REQUIRED ?= 1
BASH_SPEEDUP_REQUIRED ?= 3

bench: $(BIN)
	@SCALE='$(SCALE)' BIN='$(BIN)' DASH='$(DASH)' BASHP='$(BASHP)' ZSH='$(ZSH)' \
		ASH='$(ASH)' YASH='$(YASH)' BENCH='$(BENCH)' \
		BENCH_BASH='$(BENCH_BASH)' BENCH_KOSH='$(BENCH_KOSH)' PRIMES='$(PRIMES)' \
		PRIMES_PY='$(PRIMES_PY)' PRIMES_LIMIT='$(PRIMES_LIMIT)' PYTHON='$(PYTHON)' \
		BENCH_WARMUP_COUNT='$(BENCH_WARMUP_COUNT)' \
		BENCH_SAMPLE_COUNT='$(BENCH_SAMPLE_COUNT)' \
		DASH_SPEEDUP_REQUIRED='$(DASH_SPEEDUP_REQUIRED)' \
		BASH_SPEEDUP_REQUIRED='$(BASH_SPEEDUP_REQUIRED)' \
		$(SHELL) run-bench-test.sh

.PHONY: test clean kosh_tests refill compatdiff dashdiff bashdiff mimicrydiff bench \
		build_tests completion_tests completion_refill cli_tests highlight_tests
