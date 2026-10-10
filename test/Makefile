ifeq ($(OS), Windows_NT)
SHELL := sh
TEST_ABSOLUTE_DIRECTORY_COMMAND := pwd -W
else
SHELL := /bin/bash
TEST_ABSOLUTE_DIRECTORY_COMMAND := pwd
endif
ifeq ($(OS), Windows_NT)
TARGET ?= Windows_NT
else
TARGET ?= $(shell uname -s)
endif

ifeq ($(TARGET), Windows_NT)
SKIPPED_TESTS := \
	koshkit_filesystem_posix koshkit_inspect_posix koshkit_system_posix \
	koshkit_text_posix alias_unalias glob_dot_slash_prefix koshkit_getconf \
	pipeline_compound_group read_builtin redirect_high_fd \
	shellcheck_bashisms_sh_shebang trap umask_modes \
	bash_startup_restricted bash_xtracefd bench_behavior \
	cd_suggestion chld_trap_after_return command_edge_case_behavior \
	command_flag_forms command_p_restores_path \
	command_substitution_interrupt command_substitution_strategy \
	crlf_source ctrl_c_interrupts_bash_loop deep_nesting_guard \
	diagnostic_levels disown_removes_jobs disown_running_only \
	env_runs_command_directly exec_behavior export_function \
	export_readonly_integer fg_terminal_handoff granular_arg_caret \
	help_colors history_behavior history_noninteractive \
	interrupt_during_blocking_open job_state_running_then_stopped \
	jobspec_grammar kill_behavior kosh_identity \
	koshkit_error_notes koshkit_evil_regressions \
	koshkit_evilfiles_parity koshkit_evilio_live koshkit_eviliso \
	koshkit_file_magic_batch koshkit_stty mkdir_mode_umask \
	path_cache_behavior path_lookup_follows_changes \
	signal_noninteractive_term set_option_state \
	spawn_failure_location suspend_behavior \
	trap_during_blocking_builtin trap_startup_ignored \
	unknown_flag_color wait_on_stopped_job \
	koshkit_goodcore_archive koshkit_ls_modes koshkit_misc \
	assimilate koshkit_printenv koshkit_process_tools \
	koshkit_tail_follow koshkit_text koshkit_timeout \
	source_locations mood_diagnostics_parity \
	loop_redirect_descriptor \
	pipeline_stage_evaluation_mode shopt_states \
	subshell_replay_trap_suppression subshell_spawn_state \
	trace_policy trailing_separator_execution ulimit_validation \
	ansi_c_quoted_word complete_git_grouped_subcommands \
	complete_go_subcommands complete_help_subcommands \
	editor_append_hot_path executable_filtering_batch \
	help_options_trusted_dir_only \
	help_subcommand_second_level \
	help_subcommand_third_level \
	help_subcommands_no_colon make_targets_fallback \
	completion_glob_quoting path_quoting control_byte_quoting koshkit_mknod \
	literal_tilde_and_ansi_c_escapes \
	koshkit_live_pty koshkit_live_sanitizers koshkit_live_deltas \
	editor_ghost_menu_pty editor_kill_yank_pty editor_prompt_pty \
	editor_pending_tab_pty shell_integration_marks_pty background_terminal_pty \
	prompt_function_substitution_pty prompt_hooks_pty startup_chain_pty \
	procsub_contexts_matrix \
	shell_executable_path \
	analysis_environment_inheritance progcomp_alias_ghost koshconf_persist \
	koshconf_system_file process_substitution_release
else ifneq ($(TARGET), Linux)
SKIPPED_TESTS := koshkit_mknod koshkit_live_pty koshkit_live_sanitizers \
	koshkit_live_deltas editor_ghost_menu_pty editor_kill_yank_pty \
	editor_prompt_pty editor_pending_tab_pty background_terminal_pty \
	koshkit_evilfiles_parity \
	prompt_hooks_pty startup_chain_pty trap_during_blocking_open \
	koshconf_system_file process_substitution_release
else
SKIPPED_TESTS :=
endif
export SKIPPED_TESTS

MODE ?= dbg
ARCH ?= $(shell uname -m)
NO_TOILETLINE ?= 0
NO_KOSHKIT ?= 0
IS_NONDEBUG_BUILD := 1
ifeq ($(MODE),dbg)
IS_NONDEBUG_BUILD := 0
else ifeq ($(MODE),cosmo_dbg)
IS_NONDEBUG_BUILD := 0
endif
export IS_NONDEBUG_BUILD

ifeq ($(IS_NONDEBUG_BUILD),1)
SKIPPED_TESTS += koshkit_live_sanitizers
endif
SKIPPED_TESTS += positional_operators_matrix_warnings
ifeq ($(NO_KOSHKIT),1)
ifeq ($(KOSHKIT_BIN),)
$(error NO_KOSHKIT=1 needs KOSHKIT_BIN set to a build that bundles koshkit)
endif
export KOSHKIT_BIN
SKIPPED_TESTS += alias_unalias arith_decimal ast_process_substitution \
	bash_operators_default bash_source_stack builtin_list cd_dash \
	cd_fail_file cd_paths command compound_redirect \
	debug_err_return_traps fc fresh_evaluator_directories fresh_evaluator_jobs \
	fresh_evaluator_path glob_dot_slash_prefix \
	koshkit_compare_posix koshkit_csplit koshkit_diff koshkit_evillogs \
	koshkit_expr koshkit_filesystem_portable koshkit_filesystem_posix \
	koshkit_getconf koshkit_goodfsw koshkit_inspect_posix \
	koshkit_numeric_posix koshkit_process_posix koshkit_readlink \
	koshkit_restricted \
	koshkit_retry koshkit_stat koshkit_stream_posix koshkit_sync \
	koshkit_system_posix koshkit_terminal_posix koshkit_text_posix \
	koshkit_watch koshkit_whoami mood_arithmetic \
	process_substitution_partial_reads process_substitution_readers \
	process_substitution_traces read_builtin \
	redirect_dynamic_fd redirection_truncate_append_input \
	rerr_redirect_to_directory subshell_isolates_state test_builtin trap
endif
ifeq ($(TARGET), Linux)
ifneq ($(shell unshare -rm true 2>/dev/null && echo available),available)
SKIPPED_TESTS += koshconf_system_file
endif
endif
ifeq ($(shell command -v python3),)
SKIPPED_TESTS += koshkit_live_pty koshkit_live_deltas editor_ghost_menu_pty \
	editor_kill_yank_pty editor_prompt_pty editor_pending_tab_pty \
	shell_integration_marks_pty background_terminal_pty \
	prompt_function_substitution_pty \
	prompt_hooks_pty startup_chain_pty
endif

ifeq ($(OS), Windows_NT)
TARGET ?= Windows_NT
else
TARGET ?= $(shell uname -s)
endif
ifeq ($(TARGET), Windows_NT)
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
export TEST_HOST_PATH_SEPARATOR := :
export TEST_TEMP_DIRECTORY := .test-work
ifeq ($(TARGET), Darwin)
export TEST_TIMEOUT_SECONDS ?= 360
else
export TEST_TIMEOUT_SECONDS ?= 180
endif
export TEST_MKTEMP_DIRECTORY := $(shell $(TEST_ABSOLUTE_DIRECTORY_COMMAND))/$(TEST_TEMP_DIRECTORY)
export BIN_DIR := $(CURDIR)/bin

ifeq ($(MODE),rel)
BIN_NAME := kosh
else
BIN_NAME := kosh-$(MODE)
endif
ifeq ($(MODE),cosmo)
BIN_EXTENSION := .com
else ifeq ($(TARGET),Windows_NT)
BIN_EXTENSION := .exe
endif
BIN ?= ../$(BIN_NAME)$(BIN_EXTENSION)

ifeq ($(TARGET),Windows_NT)
export DIFF_FLAGS := -u -w -a # CRLF on Windows SMH
else
export DIFF_FLAGS := -u
endif

ifeq ($(NO_TOILETLINE), 1)
BIN_EXTENSION += -no-toiletline
endif

ifeq ($(MODE),rel)
export BIN ?= ../kosh$(BIN_EXTENSION)
else
export BIN ?= ../kosh-$(MODE)$(BIN_EXTENSION)
endif
override BIN := $(abspath $(BIN))

FAILED_LIST := failed.diff
export FAILED_LIST

export TEST_SHELL := $(SHELL)
unexport KOSH_IDENTITY KOSH_ANALYSIS KOSH_FLAGS KOSHCONF
export XDG_CONFIG_HOME := $(TEST_MKTEMP_DIRECTORY)/no-config-home
export KOSH_DEBUG_SYSTEM_KOSHCONF := $(TEST_MKTEMP_DIRECTORY)/no-system-kosh.conf

export KOSH_HISTORY_FILE := $(CURDIR)/.test_history
export KOSH_DIRECTORY_HISTORY := $(CURDIR)/.test_directory_history

ifeq ($(TARGET), Windows_NT)
export TEST_SYSTEM_PATH := $(TEST_TEMP_DIRECTORY)/system-path
else
export TEST_SYSTEM_PATH := $(dir $(shell command -v sh))
endif

export PATH := $(BIN_DIR)$(TEST_HOST_PATH_SEPARATOR)$(PATH)
export ASAN_OPTIONS := symbolize=0

export MODE
export TARGET
export NO_TOILETLINE
.PHONY: test test_suites kosh cli completion highlight sh bash test_prepare FORCE

ifeq ($(IS_NONDEBUG_BUILD),1)
DEBUG_DRIVER_SUITES :=
else
DEBUG_DRIVER_SUITES := completion highlight
endif

ifeq ($(TARGET),Windows_NT)
COMPATIBILITY_SUITES :=
else
COMPATIBILITY_SUITES := sh bash
endif

test_suites: kosh cli $(DEBUG_DRIVER_SUITES) $(COMPATIBILITY_SUITES) | test_prepare

KOSH_TESTS := $(wildcard harness/kosh/*.kosh)
kosh: $(KOSH_TESTS)

CLI_TESTS := $(wildcard harness/cli/*.sh)
cli: $(CLI_TESTS)

COMPLETION_TESTS := $(wildcard harness/completion/*.sh)
completion: $(COMPLETION_TESTS)

HIGHLIGHT_TESTS := $(wildcard harness/highlight/*.sh)
highlight: $(HIGHLIGHT_TESTS)

SH_TESTS := $(wildcard harness/sh/*.sh)
sh: $(SH_TESTS)

BASH_TESTS := $(wildcard harness/bash/*.bash)
bash: $(BASH_TESTS)

harness/kosh/%.kosh: FORCE $(BIN) | test_prepare
	@$(SHELL) $(BIN_DIR)/run-test '$@' $(BIN_DIR)/run-harness-kosh

harness/cli/%.sh: FORCE $(BIN) | test_prepare
	@$(SHELL) $(BIN_DIR)/run-test '$@' $(BIN_DIR)/run-harness-script

harness/completion/%.sh: FORCE $(BIN) | test_prepare
	@$(SHELL) $(BIN_DIR)/run-test '$@' $(BIN_DIR)/run-harness-script

harness/highlight/%.sh: FORCE $(BIN) | test_prepare
	@$(SHELL) $(BIN_DIR)/run-test '$@' $(BIN_DIR)/run-harness-script

harness/sh/%.sh: FORCE $(BIN) | test_prepare
	@$(SHELL) $(BIN_DIR)/run-test '$@' $(BIN_DIR)/run-harness-sh

harness/bash/%.bash: FORCE $(BIN) | test_prepare
	@$(SHELL) $(BIN_DIR)/run-test '$@' $(BIN_DIR)/run-harness-bash

FORCE:

clean:
	@rm -f $(FAILED_LIST) $(KOSH_HISTORY_FILE) $(KOSH_DIRECTORY_HISTORY)
	@rm -rf "$(CURDIR)/$(TEST_TEMP_DIRECTORY)"

test_prepare:
	@$(BIN_DIR)/invoke-koshkit rm -f '$(FAILED_LIST)'
	@$(BIN_DIR)/invoke-koshkit touch '$(FAILED_LIST)'
	@$(BIN_DIR)/invoke-koshkit rm -rf '$(CURDIR)/$(TEST_TEMP_DIRECTORY)'

test:
	@SUITES_STATUS=0; \
	$(MAKE) -k test_suites || SUITES_STATUS=$$?; \
	if test -s '$(FAILED_LIST)'; then \
		cat -n '$(FAILED_LIST)'; \
	fi; \
	if test "$$SUITES_STATUS" -ne 0; then \
		printf 'test: the suites stopped with status %s\n' \
			"$$SUITES_STATUS" >&2; \
		exit "$$SUITES_STATUS"; \
	fi; \
	if test -s '$(FAILED_LIST)'; then \
		exit 1; \
	fi

REFILL ?=

refill: $(BIN)
	@REFILL='$(REFILL)' \
		$(SHELL) $(BIN_DIR)/run-refill '$(SHELL)'

export ZSH ?= /bin/zsh
export ASH ?= /bin/busybox
export YASH ?= $(shell command -v yash)
export DASH ?= $(shell command -v dash)
export BASHP ?= $(shell ../scripts/find-modern-bash.sh)
export PYTHON ?= python3

export PRIMES_LIMIT ?= 100000
export SCALE ?= 100
export BENCH_WARMUP_COUNT ?= 0
export BENCH_SAMPLE_COUNT ?= 1
export DASH_SPEEDUP_REQUIRED ?= 1
export BASH_SPEEDUP_REQUIRED ?= 3

bench: $(BIN)
	@$(SHELL) $(BIN_DIR)/run-bench

.PHONY: test clean refill bench
