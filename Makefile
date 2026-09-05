ifndef VERBOSE
MAKEFLAGS += -s
endif

.DEFAULT_GOAL := kosh

CPU_COUNT := $(shell ./scripts/cpu-count.sh)

MAKE_COMMAND_LINE ?= $(shell \
	if test -r /proc/$${PPID}/cmdline; then \
		tr '\000' ' ' < /proc/$${PPID}/cmdline 2>/dev/null; \
	elif command -v ps >/dev/null 2>&1; then \
		ps -p $${PPID} -o command= 2>/dev/null; \
	elif command -v powershell.exe >/dev/null 2>&1; then \
		powershell.exe -NoProfile -Command \
			"(Get-CimInstance Win32_Process -Filter 'ProcessId=$${PPID}').CommandLine" \
			2>/dev/null; \
	fi)
CALLER_JOBS := $(filter -j% --jobs% --jobserver%,$(MAKEFLAGS)) \
	$(filter -j% --jobs%,$(MAKE_COMMAND_LINE)) \
	$(if $(filter command line,$(origin MAKE_COMMAND_LINE)),explicit-command-line,)
AUTO_JOBS = $(if $(strip $(CALLER_JOBS)),,-j$(CPU_COUNT))

MODE ?= dbg
NO_TOILETLINE ?= 0

ifeq ($(OS), Windows_NT)
TARGET ?= Windows_NT
else
TARGET ?= $(shell uname -s)
endif

export MODE
export NO_TOILETLINE
export TARGET

TEST_TARGET := $(if $(filter 1,$(NO_TOILETLINE)),cli_history_noninteractive,test)

all: kosh test

kosh:
	echo Creating kosh...
	$(MAKE) $(AUTO_JOBS) -C src kosh

install:
	echo Installing...
	$(MAKE) $(AUTO_JOBS) -C src install

uninstall:
	echo Uninstalling...
	$(MAKE) $(AUTO_JOBS) -C src uninstall

tidy:
	echo Launching '$$'CLANG_TIDY...
	$(MAKE) $(AUTO_JOBS) -C src tidy

fmt:
	echo Launching '$$'CLANG_FMT...
	$(MAKE) $(AUTO_JOBS) -C src fmt

test: kosh
	echo Launching tests...
	$(MAKE) $(AUTO_JOBS) -C test $(TEST_TARGET)

bench: kosh
	echo Launching benchmarks...
	$(MAKE) -C test bench

refill_tests: kosh
	echo Refilling tests...
	$(MAKE) $(AUTO_JOBS) -C test refill

clean:
	echo Cleaning up...
	$(MAKE) $(AUTO_JOBS) -C src clean
	$(MAKE) $(AUTO_JOBS) -C test clean

.PHONY: all kosh install uninstall tidy fmt test bench refill_tests clean
