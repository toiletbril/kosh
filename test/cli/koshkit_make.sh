# The barebones make reads a Makefile, expands its variables, and runs each
# recipe through the shell. The recipe lines echo before they run, and a @ prefix
# silences that echo.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1

cat > Makefile <<'EOF'
CC = echo
GREETING = hello

all: greet done_marker

greet:
	$(CC) $(GREETING) from make

done_marker:
	@$(CC) finished
EOF

echo "--- default goal ---"
"$BIN" -c 'koshkit make'
echo "--- explicit target ---"
"$BIN" -c 'koshkit make greet'
echo "--- missing target ---"
"$BIN" -c 'koshkit make nope' 2>&1
echo "rc=$?"

# The := immediate expansion breaks the MAKE := $(MAKE) self-reference, the
# $(wildcard) function lists files, a $(VAR:a=b) substitution reference maps
# them, an ifeq conditional selects a value, a backslash continuation joins a
# multi-line value, a prerequisite builds first, and each recipe line runs in its
# own subshell so a forking command does not abandon the line after it.
: > a.c
: > b.c
cat > Makefile <<'EOF'
MODE ?= dbg
MAKE := $(MAKE) -j4
SRC := $(wildcard *.c)
OBJ := $(SRC:%.c=%.o)
FLAGS := \
	one \
	two
ifeq ($(MODE), dbg)
TAG := debug
else
TAG := other
endif

all: dirs
	@echo "make=[$(MAKE)]"
	@echo "src=[$(SRC)]"
	@echo "obj=[$(OBJ)]"
	@echo "flags=[$(FLAGS)]"
	@echo "tag=[$(TAG)]"

dirs:
	@mkdir -p deep
	@echo "second recipe line ran after a forking command"
EOF
echo "--- advanced features ---"
"$BIN" -c 'koshkit make' 2>&1

echo "--- ignored flags -B and -k ---"
cat > Makefile <<'EOF'
all:
	@echo built
EOF
"$BIN" -c 'koshkit make -B -k all' 2>&1
echo "rc=$?"

echo "--- failing recipe aborts ---"
cat > Makefile <<'EOF'
broken:
	@false
	@echo "this line must not run"
EOF
"$BIN" -c 'koshkit make broken' 2>&1
echo "rc=$?"

# The barebones make resolves a pattern rule when no explicit rule names the
# goal, deriving the stem from the % and filling the automatic variables $@, $<,
# and $^ in the recipe.
unset KOSH_FLAGS
BIN=$(CDPATH= cd -- "$(dirname -- "$BIN")" && pwd)/$(basename -- "$BIN")
d=$(mktemp -d) || exit 1
cd "$d" || exit 1

printf 'x\n' > foo.c
printf 'y\n' > bar.c

cat > Makefile <<'EOF'
CC = echo cc

all: foo.o bar.o

%.o: %.c
	$(CC) $< -o $@
EOF

echo "--- default goal builds both through the pattern ---"
"$BIN" -c 'koshkit make'
echo "--- explicit object target ---"
"$BIN" -c 'koshkit make foo.o'
echo "--- all prerequisites variable ---"
cat > Makefile <<'EOF'
report: a b c
	echo all are $^ and first is $<
EOF
: > a
: > b
: > c
"$BIN" -c 'koshkit make'

unset KOSH_FLAGS
# A command that fails inside a $(shell ...) of a koshkit makefile reports the
# error with the make filename rather than a bare unnamed line. The temp
# directory is left in place so the test never runs rm.
dir=$(mktemp -d)
printf 'V := $(shell nonexistent_prog_zzz)\nall:\n\techo $(V)\n' > "$dir/Makefile"
echo "== the \$(shell) error names the make source (count):"
"$BIN" -c "cd '$dir'; koshkit make" 2>&1 | grep -c "make:.*Program 'nonexistent_prog_zzz' wasn't found"
