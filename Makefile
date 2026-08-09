ifndef VERBOSE
MAKEFLAGS += -s
endif

.DEFAULT_GOAL := shit

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

ifeq ($(OS), Windows_NT)
TARGET ?= Windows_NT
else
TARGET ?= $(shell uname -s)
endif

export MODE
export TARGET

all: shit test

shit:
	echo Creating shit...
	$(MAKE) $(AUTO_JOBS) -C src shit

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

test: shit
	echo Launching tests...
	$(MAKE) $(AUTO_JOBS) -C test test

bench: shit
	echo Launching benchmarks...
	$(MAKE) -C test bench

refill_tests: shit
	echo Refilling tests...
	$(MAKE) $(AUTO_JOBS) -C test refill

clean:
	echo Cleaning up...
	$(MAKE) $(AUTO_JOBS) -C src clean
	$(MAKE) $(AUTO_JOBS) -C test clean

.PHONY: all shit install uninstall tidy fmt test bench refill_tests clean
