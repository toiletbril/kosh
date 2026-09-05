# make MODE=dbg/rel/prof/cosmo/cosmo_dbg
MODE ?= dbg
TARGET ?= # Linux/Windows_NT/Darwin
ARCH ?= $(shell uname -m)
NO_TOILETLINE ?= 0

ROUTED_SOURCES := PlatformPosix.cpp PlatformPosixExtra.cpp \
	PlatformPosixFilesystem.cpp PlatformPosixProcess.cpp PlatformWin32.cpp \
	PlatformWin32Filesystem.cpp PlatformWin32Process.cpp
SOURCES := $(filter-out $(ROUTED_SOURCES),$(wildcard *.cpp))
HEADERS := $(wildcard *.hpp)

SOURCE_DIRECTORIES := $(wildcard */.)

SOURCES += $(wildcard */**.cpp)
HEADERS += $(wildcard */**.hpp)

# The host OS, Windows_NT on Windows and the uname elsewhere.
ifeq ($(OS), Windows_NT)
HOST_OPERATING_SYSTEM := Windows_NT
else
HOST_OPERATING_SYSTEM := $(shell uname -s)
endif

IS_ANDROID_HOST := $(if $(findstring Android,$(shell uname -o 2>/dev/null)),1,0)

# Windows_NT/Linux/Darwin
ifeq ($(TARGET),)
TARGET := $(HOST_OPERATING_SYSTEM)
endif

# Validate TARGET
ifneq ($(TARGET), Linux)
ifneq ($(TARGET), Windows_NT)
ifneq ($(TARGET), Darwin)
$(error Unknown target: $(TARGET))
endif
endif
endif

ifeq ($(TARGET), Windows_NT)
REGEX_SOURCES := regcomp regerror regexec tre-mem
endif

ifeq ($(TARGET), Linux)
ifneq ($(HOST_OPERATING_SYSTEM), Linux)
ifneq ($(MODE), rel)
$(error Linux cross-compilation only supports MODE=rel)
endif
ifneq ($(filter arm64 aarch64,$(ARCH)),)
LINUX_TARGET_ARCHITECTURE := aarch64
else ifeq ($(ARCH), x86_64)
LINUX_TARGET_ARCHITECTURE := x86_64
else
$(error Linux cross-compilation does not support ARCH='$(ARCH)')
endif
IS_LINUX_CROSS_COMPILATION := 1
endif
endif

SHOULD_LINK_STATIC_LINUX := 0
ifeq ($(TARGET), Linux)
ifneq ($(IS_ANDROID_HOST), 1)
SHOULD_LINK_STATIC_LINUX := 1
endif
endif

BUILD_VARIANT_SUFFIX := $(if $(filter 1,$(NO_TOILETLINE)),-no-toiletline,)

ifeq ($(IS_LINUX_CROSS_COMPILATION), 1)
OBJ_DIR ?= o/$(TARGET)/$(LINUX_TARGET_ARCHITECTURE)/$(MODE)$(BUILD_VARIANT_SUFFIX)
else
OBJ_DIR ?= o/$(TARGET)/$(MODE)$(BUILD_VARIANT_SUFFIX)
endif
OBJECT_DIRECTORY := $(OBJ_DIR)

REGEX_OBJECTS := $(REGEX_SOURCES:%=$(OBJECT_DIRECTORY)/vendor/regex/%.o)
OBJECTS := $(SOURCES:%.cpp=$(OBJECT_DIRECTORY)/%.o) $(REGEX_OBJECTS)
DEPENDENCY_FILES := $(OBJECTS:.o=.d)

MODE_SUFFIX := $(if $(filter rel,$(MODE)),,-$(MODE))
ifeq ($(MODE), cosmo)
OUTPUT_PATH := ../kosh$(MODE_SUFFIX).com
else ifeq ($(TARGET), Windows_NT)
OUTPUT_PATH := ../kosh$(MODE_SUFFIX).exe
else
OUTPUT_PATH := ../kosh$(MODE_SUFFIX)
endif

LINKED_OUTPUT_PATH := $(OBJECT_DIRECTORY)/$(notdir $(OUTPUT_PATH))

# Cosmopolitan/Clang.
CXX_VARIABLE_ORIGIN := $(origin CXX)
ifneq ($(filter cosmo cosmo_dbg,$(MODE)),)
CXX := cosmoc++
else ifeq ($(OS), Windows_NT)
ifeq ($(CXX_VARIABLE_ORIGIN), default)
CXX := clang++
endif
else ifeq ($(CXX_VARIABLE_ORIGIN), default)
CXX := clang++
endif

# If we're cross-compiling, use MingW.
ifeq ($(TARGET), Windows_NT)
ifneq ($(OS), Windows_NT)
CXX := x86_64-w64-mingw32-g++
endif
endif

ifeq ($(TARGET), Windows_NT)
WINDOWS_COMPILER_TARGET := $(shell $(CXX) -dumpmachine 2>/dev/null)
IS_WINDOWS_GNU_ABI := $(or $(findstring mingw,$(WINDOWS_COMPILER_TARGET)),\
	$(findstring windows-gnu,$(WINDOWS_COMPILER_TARGET)))
endif

ifeq ($(TARGET), Linux)
ifneq ($(HOST_OPERATING_SYSTEM), Linux)
CXX := zig c++ -target $(LINUX_TARGET_ARCHITECTURE)-linux-musl
endif
endif

ifeq ($(TARGET), Darwin)
ifneq ($(HOST_OPERATING_SYSTEM), Darwin)
ifeq ($(CXX_VARIABLE_ORIGIN), default)
ifeq ($(ARCH), x86_64)
$(error x86_64 Darwin build is not supported)
else
CXX := oa64-clang++
endif
endif
endif
endif

ifeq ($(MODE), cosmo)
OS_INFO := "unknown-cosmo"
LIBC_INFO := cosmopolitan libc
else ifeq ($(MODE), cosmo_dbg)
OS_INFO := "unknown-cosmo-dbg"
LIBC_INFO := cosmopolitan libc
else ifeq ($(IS_LINUX_CROSS_COMPILATION), 1)
OS_INFO := Linux $(LINUX_TARGET_ARCHITECTURE) cross
else
OS_INFO := $(shell test -f /etc/os-release && . /etc/os-release 2>/dev/null; printf '%s%s' "$${PRETTY_NAME:+$$PRETTY_NAME }" "$$(uname -rvom 2>/dev/null || echo '<no uname>')")
endif
OPERATING_SYSTEM_INFORMATION := $(OS_INFO)

ifndef LIBC_INFO
ifeq ($(TARGET), Windows_NT)
ifneq ($(IS_WINDOWS_GNU_ABI),)
LIBC_INFO := mingw msvcrt
else
LIBC_INFO := msvc
endif
else ifeq ($(TARGET), Darwin)
LIBC_INFO := Apple libSystem
else ifeq ($(IS_ANDROID_HOST), 1)
LIBC_INFO := Android bionic
else ifneq ($(TARGET), $(HOST_OPERATING_SYSTEM))
LIBC_INFO := zig musl libc++
else
LIBC_INFO := $(shell getconf GNU_LIBC_VERSION 2>/dev/null || { command -v ldd >/dev/null 2>&1 && ldd --version 2>&1 | head -n 1; } || echo 'unknown libc')
endif
endif
LIBC_INFORMATION := $(LIBC_INFO)

COMMIT_HASH := $(shell git rev-parse HEAD || echo "<no git>")

# Remember CXXFLAGS passed to us from CLI.
ENVCXXFLAGS := $(CXXFLAGS)
USER_CXXFLAGS := $(ENVCXXFLAGS)
CXXFLAGS := # nothing

encode_build_metadata = $(1)
ifneq ($(filter cosmo cosmo_dbg,$(MODE)),)
EMPTY_STRING :=
SPACE := $(EMPTY_STRING) $(EMPTY_STRING)
encode_build_metadata = $(subst $(SPACE),\\040,$(1))
endif

BUILD_DEFINITIONS := \
	-DKOSH_COMPILER_COMMAND="\"$(call encode_build_metadata,$(CXX))\"" \
	-DKOSH_LIBC="\"$(call encode_build_metadata,$(LIBC_INFORMATION))\"" \
	-DKOSH_OS_INFO="\"$(call encode_build_metadata,$(OPERATING_SYSTEM_INFORMATION))\"" \
	-DKOSH_COMMIT_HASH="\"$(call encode_build_metadata,$(COMMIT_HASH))\"" \
	-DKOSH_BUILD_MODE="\"$(call encode_build_metadata,$(MODE))\"" \
	-DKOSH_ENVCXXFLAGS="\"$(call encode_build_metadata,$(USER_CXXFLAGS))\"" \
	$(if $(filter 1,$(NO_TOILETLINE)),-DKOSH_NO_TOILETLINE,)

CXXFLAGS := \
	-Wall \
	-Wextra \
	-Wdouble-promotion \
	-Wno-format \
	-Werror \
	-Wno-unused-command-line-argument \
	-Wno-deprecated \
	-std=c++23 \
	-Wno-unknown-warning-option \
	-Wno-error=maybe-uninitialized \
	-pipe \
	-fvisibility=hidden \
	-fno-rtti \
	-fno-semantic-interposition \
	-Wno-unknown-pragmas \
	-fno-strict-aliasing \
	$(BUILD_DEFINITIONS) \
	$(USER_CXXFLAGS)
ifeq ($(IS_LINUX_CROSS_COMPILATION), 1)
CXXFLAGS += -Wno-date-time
endif

ifeq ($(TARGET), Linux)
CXXFLAGS += -Wl,-Bsymbolic
endif

LINK_LIBRARIES := # nothing

ifeq ($(TARGET), Windows_NT)
ifeq ($(IS_WINDOWS_GNU_ABI),)
LINK_LIBRARIES += --rtlib=compiler-rt
endif
endif

AVAILABLE_LLD := $(shell command -v ld.lld 2>/dev/null || command -v lld 2>/dev/null)
ifeq ($(TARGET), Windows_NT)
ifneq ($(AVAILABLE_LLD),)
USE_LD := -fuse-ld=lld
else
USE_LD :=
endif
else ifeq ($(TARGET), Darwin)
# mold is ELF-only and lld-as-Mach-O is rare on macOS, so the system ld64
# links directly with no -fuse-ld flag.
USE_LD :=
else ifneq ($(shell command -v mold 2>/dev/null),)
USE_LD := -fuse-ld=mold
else ifneq ($(AVAILABLE_LLD),)
USE_LD := -fuse-ld=lld
else
USE_LD :=
endif
LINKER_SELECTION_FLAG := $(USE_LD)

# MODE=rel
ifeq ($(MODE), rel)
CXXFLAGS += \
	-DNDEBUG \
	-O3 \
	-U_FORTIFY_SOURCE \
	-D_FORTIFY_SOURCE=2
ifneq ($(TARGET), Windows_NT)
ifneq ($(TARGET), Darwin)
CXXFLAGS += -flto
else ifeq ($(HOST_OPERATING_SYSTEM), Darwin)
CXXFLAGS += -flto
endif
endif
ifeq ($(TARGET), Linux)
ifeq ($(IS_LINUX_CROSS_COMPILATION), 1)
CXXFLAGS += -static
else ifeq ($(SHOULD_LINK_STATIC_LINUX), 1)
CXXFLAGS += \
	-static-pie \
	-nostdlib++
LINK_LIBRARIES += -lsupc++
endif
endif
# MODE=dbg
else ifeq ($(MODE), dbg)
CXXFLAGS += \
	-fno-omit-frame-pointer \
	-g3 \
	-O0
ifneq ($(TARGET), Windows_NT)
ifneq ($(MODE), cosmo)
CXXFLAGS += \
	-fsanitize=address \
	-fsanitize=undefined
endif
endif
# MODE=cosmo
else ifeq ($(MODE), cosmo)
CXXFLAGS += \
	-mcosmo \
	-mtiny \
	-fexceptions \
	-g \
	-DNDEBUG \
	-O3
ifneq ($(CXX), cosmoc++)
$(error Your CXX is "$(CXX)" instead of "cosmoc++" for MODE=cosmo)
endif
# MODE=cosmo_dbg
else ifeq ($(MODE), cosmo_dbg)
CXXFLAGS += \
	-mcosmo \
	-mdbg \
	-fexceptions \
	-g3
ifneq ($(CXX), cosmoc++)
$(error Your CXX is "$(CXX)" instead of "cosmoc++" for MODE=cosmo_dbg)
endif
# MODE=prof
else ifeq ($(MODE), prof)
CXXFLAGS += \
	-g3 \
	-DNDEBUG \
	-fno-omit-frame-pointer \
	-flto
ifneq ($(TARGET), Windows_NT)
CXXFLAGS += \
	-O3 \
	-U_FORTIFY_SOURCE \
	-D_FORTIFY_SOURCE=2
ifeq ($(SHOULD_LINK_STATIC_LINUX), 1)
CXXFLAGS += -static-pie
endif
else
CXXFLAGS += \
	-O2
endif
ifeq ($(TARGET), Linux)
CXXFLAGS += -finstrument-functions
ifeq ($(SHOULD_LINK_STATIC_LINUX), 1)
CXXFLAGS += -nostdlib++
LINK_LIBRARIES += -lsupc++
endif
endif
else ifeq ($(MODE), cov)
COVERAGE_PROFILE_DIRECTORY := $(abspath ../coverage/profraw)
CXXFLAGS += \
	-g3 \
	-DNDEBUG \
	-fno-omit-frame-pointer \
	-O0 \
	"-fprofile-instr-generate=$(COVERAGE_PROFILE_DIRECTORY)/kosh-%p.profraw" \
	-fcoverage-mapping
ifeq ($(TARGET), Linux)
ifeq ($(SHOULD_LINK_STATIC_LINUX), 1)
CXXFLAGS += \
	-static-pie \
	-nostdlib++
LINK_LIBRARIES += -lsupc++
endif
endif
else
$(error Invalid MODE=$(MODE). Available modes are "dbg", "rel", "prof", "cov", "cosmo", "cosmo_dbg")
endif

ifneq ($(TARGET), Windows_NT)
CXXFLAGS += \
	-pthread
ifneq ($(IS_ANDROID_HOST), 1)
LINK_LIBRARIES += -lpthread
endif
endif

# Missing Windows libraries.
ifeq ($(TARGET), Windows_NT)
LINK_LIBRARIES += \
	-luserenv \
	-ladvapi32 \
	-lrstrtmgr
endif

ifeq ($(TARGET), Windows_NT)
CXXFLAGS += \
	-Wno-unknown-pragmas
ifneq ($(IS_WINDOWS_GNU_ABI),)
CXXFLAGS += \
		-mconsole \
		-static \
		-static-libgcc \
		-static-libstdc++
LINK_LIBRARIES += -municode
else
LINK_LIBRARIES += -Wl,/entry:wmainCRTStartup
endif
endif

REGEX_COMPILER_FLAGS := $(filter-out -std=c++23 -fno-rtti,$(CXXFLAGS)) -std=c11 \
	-I../vendor/regex

# A bare `make` builds the shell, the same as `make kosh`, rather than the first
# object the rules below would otherwise name as the default goal.
.DEFAULT_GOAL := kosh

COMMIT_HASH_STAMP := $(OBJECT_DIRECTORY)/.commit-hash
COMMIT_HASH_OBJECTS := \
	$(OBJECT_DIRECTORY)/Cli.o \
	$(OBJECT_DIRECTORY)/Main.o
STORED_COMMIT_HASH := $(shell cat $(COMMIT_HASH_STAMP) 2>/dev/null)

force_commit_hash:

force_commit_hash_objects:

ifneq ($(STORED_COMMIT_HASH),$(COMMIT_HASH))
$(COMMIT_HASH_OBJECTS): force_commit_hash_objects
endif

$(COMMIT_HASH_STAMP): force_commit_hash | dirs
	current_commit_hash='$(COMMIT_HASH)'; \
	if test ! -f $@ || test "$$(cat $@)" != "$$current_commit_hash"; then \
		printf '%s\n' "$$current_commit_hash" > $@; \
	fi

$(COMMIT_HASH_OBJECTS): $(COMMIT_HASH_STAMP)

# Every object is built into $(OBJECT_DIRECTORY), so its directory tree must exist first.
# The order-only prerequisite creates it before any compile, so a plain
# `make MODE=rel` from a clean tree works without first running the kosh target.
$(OBJECTS): | dirs

$(OBJECT_DIRECTORY)/%.o: %.cpp Makefile
	printf "\tCompiling %-42s $@... %-12s\r" $< ' '
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(OBJECT_DIRECTORY)/vendor/regex/%.o: ../vendor/regex/%.c Makefile
	printf "\tCompiling %-42s $@... %-12s\r" $< ' '
	$(CXX) $(REGEX_COMPILER_FLAGS) -x c -MMD -MP -c $< -o $@

# Linking.
$(LINKED_OUTPUT_PATH): $(OBJECTS)
	printf "\tLinking $(LINKED_OUTPUT_PATH)... %-64s\r" ' '
ifeq ($(TARGET), Windows_NT)
	cd $(OBJECT_DIRECTORY) && $(CXX) $(CXXFLAGS) $(LINKER_SELECTION_FLAG) \
		$(patsubst $(OBJECT_DIRECTORY)/%,%,$(OBJECTS)) $(LINK_LIBRARIES) \
		-o $(abspath $@)
else
	$(CXX) $(CXXFLAGS) $(LINKER_SELECTION_FLAG) $(OBJECTS) $(LINK_LIBRARIES) -o $@
endif

dirs: $(SOURCE_DIRECTORIES)
	mkdir -p $(OBJECT_DIRECTORY)/
	for source_directory in $(SOURCE_DIRECTORIES); do \
		mkdir -p "$(OBJECT_DIRECTORY)/$$source_directory"; \
	done
ifneq ($(REGEX_SOURCES),)
	mkdir -p $(OBJECT_DIRECTORY)/vendor/regex
endif
ifeq ($(MODE), cov)
	mkdir -p "$(COVERAGE_PROFILE_DIRECTORY)"
endif

kosh: dirs
	$(MAKE) $(LINKED_OUTPUT_PATH)
ifneq ($(LINKED_OUTPUT_PATH), $(OUTPUT_PATH))
	cp $(LINKED_OUTPUT_PATH) $(OUTPUT_PATH)
endif

# INSTALL_MOOD_SYMLINKS="sh bash dash".
PREFIX ?= /usr/local
DESTDIR ?=
bindir ?= $(PREFIX)/bin
mandir ?= $(PREFIX)/share/man/man1
man5dir ?= $(PREFIX)/share/man/man5
bashcompdir ?= $(PREFIX)/share/bash-completion/completions
INSTALL_MOOD_SYMLINKS ?=

RELEASE_BINARY := ../kosh
COMMAND_MANPAGE := ../docs/kosh.1
CONFIGURATION_MANPAGE := ../docs/kosh.5
BASH_COMPLETION_FILE := ../completions/kosh.bash

install:
ifeq ($(TARGET), Windows_NT)
	$(error Unable to figure out install directory for Windows. \
			Please put '$(realpath ..)' in your PATH manually)
else
	$(MAKE) MODE=rel kosh
	mkdir -p "$(DESTDIR)$(bindir)" "$(DESTDIR)$(mandir)" \
		"$(DESTDIR)$(man5dir)" "$(DESTDIR)$(bashcompdir)"
	install -m 755 $(RELEASE_BINARY) "$(DESTDIR)$(bindir)/kosh"
	install -m 644 $(COMMAND_MANPAGE) "$(DESTDIR)$(mandir)/kosh.1"
	install -m 644 $(CONFIGURATION_MANPAGE) "$(DESTDIR)$(man5dir)/kosh.5"
	install -m 644 $(BASH_COMPLETION_FILE) "$(DESTDIR)$(bashcompdir)/kosh"
	for mood in $(INSTALL_MOOD_SYMLINKS); do \
		ln -sf kosh "$(DESTDIR)$(bindir)/$$mood"; \
	done
	echo "Installed kosh, its manual pages, and its bash completion under '$(DESTDIR)$(PREFIX)'."
endif

uninstall:
	rm -f "$(DESTDIR)$(bindir)/kosh"
	rm -f "$(DESTDIR)$(mandir)/kosh.1"
	rm -f "$(DESTDIR)$(man5dir)/kosh.5"
	rm -f "$(DESTDIR)$(bashcompdir)/kosh"
	for mood in $(INSTALL_MOOD_SYMLINKS); do \
		rm -f "$(DESTDIR)$(bindir)/$$mood"; \
	done
	echo "Removed kosh, its manual pages, and its bash completion from '$(DESTDIR)$(PREFIX)'."

CLANG_TIDY ?= clang-tidy

tidy:
	echo "	CLANG_TIDY $(SOURCES)"
	$(CLANG_TIDY) $(SOURCES) --header-filter='.*[.](cpp|hpp)$$' -- $(CXXFLAGS)

CLANG_FORMAT ?= clang-format

fmt:
	echo "	CLANG_FORMAT -i $(SOURCES) $(HEADERS)"
	$(CLANG_FORMAT) -i $(SOURCES) $(ROUTED_SOURCES) $(HEADERS)

clean:
	rm -rf ./o ../kosh ../kosh-$(MODE) ../kosh.exe ../kosh-$(MODE).exe
	rm -rf ../*.ilk ../*.pdb ../*.lib # Windows
	rm -rf ../kosh.com ../kosh-cosmo.com ../kosh.com.dbg ../kosh-cosmo.com.dbg ../kosh.aarch64.elf ../kosh-cosmo.aarch64.elf # Cosmopolitan
	rm -rf ../perf*
	rm -rf ../zig-out
	rm -rf ../*.profraw ../*.profdata ../coverage # Coverage

.PHONY: dirs force_commit_hash force_commit_hash_objects kosh install uninstall tidy fmt clean

-include $(DEPENDENCY_FILES)
