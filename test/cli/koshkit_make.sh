# The barebones make reads a Makefile, expands its variables, and runs each
# recipe through the shell. The recipe lines echo before they run, and a @ prefix
# silences that echo.
unset KOSH_FLAGS MAKEFLAGS MFLAGS MAKELEVEL
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

echo "--- makefile selection and macro precedence ---"
cat > Makefile <<'EOF'
VALUE = makefile

all:
	@echo "value=$(VALUE)"
EOF
"$BIN" -c 'koshkit make VALUE=command'
VALUE=environment "$BIN" -c 'koshkit make'
VALUE=environment "$BIN" -c 'koshkit make -e'
cat > first.mk <<'EOF'
VALUE = first

first:
	@echo "first=$(VALUE)"
EOF
cat > second.mk <<'EOF'
VALUE = second

all: first
	@case "$(MAKEFLAGS)" in (*B*s*|*s*B*) echo makeflags=yes;; (*) echo makeflags=no;; esac
EOF
"$BIN" -c 'koshkit make -B -s -f first.mk -f second.mk all'
cat > grouped.mk <<'EOF'
all:
	@echo grouped-file
EOF
"$BIN" -c 'koshkit make -sf grouped.mk'
echo "--- options after operands ---"
"$BIN" -c 'koshkit make -f grouped.mk all -s'

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

echo "--- freshness and query modes ---"
cat > Makefile <<'EOF'
target: prerequisite
	@echo rebuilt
EOF
: > prerequisite
: > target
touch -t 202001010000 prerequisite
touch -t 202101010000 target
"$BIN" -c 'koshkit make target'
echo "fresh=$?"
"$BIN" -c 'koshkit make -q target'
echo "fresh-query=$?"
"$BIN" -c 'koshkit make -B target'
echo "forced=$?"
touch -t 202201010000 prerequisite
"$BIN" -c 'koshkit make -q target'
echo "stale-query=$?"
"$BIN" -c 'koshkit make target'
echo "stale=$?"

echo "--- execution modes ---"
cat > Makefile <<'EOF'
dry:
	@echo dry > dry-marker

silent:
	echo visible

touched:
	@echo recipe-must-not-run
EOF
"$BIN" -c 'koshkit make -n dry'
[ -e dry-marker ] && echo dry-created=yes || echo dry-created=no
"$BIN" -c 'koshkit make -s silent'
"$BIN" -c 'koshkit make -t touched'
[ -e touched ] && echo touched=yes || echo touched=no

echo "--- failing recipe aborts ---"
cat > Makefile <<'EOF'
broken:
	@false
	@echo "this line must not run"

independent:
	@echo independent

compound:
	@false; echo compound-must-not-run

compound-ignored:
	-@false; echo compound-ignored
EOF
"$BIN" -c 'koshkit make broken' 2>&1
echo "rc=$?"
"$BIN" -c 'koshkit make -i broken'
echo "ignore=$?"
"$BIN" -c 'koshkit make -k broken independent' 2> "$TEST_NULL_DEVICE"
echo "keep-going=$?"
"$BIN" -c 'koshkit make -k -S broken independent' 2> "$TEST_NULL_DEVICE"
echo "stop=$?"
"$BIN" -c 'koshkit make -S -k broken independent' 2> "$TEST_NULL_DEVICE"
echo "keep-last=$?"
"$BIN" -c 'koshkit make compound' 2> "$TEST_NULL_DEVICE"
echo "compound=$?"
"$BIN" -c 'koshkit make compound-ignored'
echo "compound-ignored=$?"
cat > Makefile <<'EOF'
interrupted:
	@: > interrupted
	@kill -INT $$$$
EOF
"$BIN" -c 'koshkit make interrupted' 2> "$TEST_NULL_DEVICE"
echo "interrupt=$?"
[ -e interrupted ] && echo interrupt-cleanup=no || echo interrupt-cleanup=yes

echo "--- special targets and suffix inference ---"
cat > Makefile <<'EOF'
.SILENT:
.IGNORE:
.DEFAULT:
	echo "default-$@ first=$<"

all: generated
	echo finished

generated:
	false
	echo after-error
EOF
"$BIN" -c 'koshkit make all'
"$BIN" -c 'koshkit make fallback'
cat > Makefile <<'EOF'
.SUFFIXES: .src .out

.src.out:
	echo "suffix $< $@"

all: item.out
EOF
: > item.src
"$BIN" -c 'koshkit make all'
cat > Makefile <<'EOF'
CC = echo compile
CFLAGS = -O9
EOF
: > builtin.c
"$BIN" -c 'koshkit make builtin.o'
"$BIN" -c 'koshkit make -r builtin.o' 2> "$TEST_NULL_DEVICE"
echo "no-builtins=$?"
printf '#!/bin/sh\necho copied-shell\n' > builtin-script.sh
"$BIN" -c 'koshkit make -s builtin-script'
[ -x builtin-script ] && echo shell-rule=yes || echo shell-rule=no
mv Makefile configured.mk
: > standalone.c
"$BIN" -c 'koshkit make CC=echo standalone.o'
mv configured.mk Makefile

echo "--- parsing automatic variables and cleanup ---"
cat > Makefile <<'EOF'
HASH = value\#tail

one two: shared
	@echo "target=$@ hash=$(HASH)"

shared:
	@:

joined: first
	@echo "joined=$^ newer=$?"
joined: second

inline: ; @echo inline

posix:
	@echo {one,two}

failed:
	@: > failed
	@false

.PRECIOUS: precious
precious:
	@: > precious
	@false
EOF
: > shared
: > first
: > second
: > joined
touch -t 202001010000 first joined
touch -t 202201010000 second
"$BIN" -c 'koshkit make -B one two'
"$BIN" -c 'koshkit make joined'
"$BIN" -c 'koshkit make inline'
"$BIN" -c 'koshkit make posix'
"$BIN" -c 'koshkit make failed' 2> "$TEST_NULL_DEVICE"
[ -e failed ] && echo failed-kept=yes || echo failed-kept=no
"$BIN" -c 'koshkit make precious' 2> "$TEST_NULL_DEVICE"
[ -e precious ] && echo precious-kept=yes || echo precious-kept=no

echo "--- POSIX database stdin and target attributes ---"
cat > Makefile <<'EOF'
VALUE = printed

print:
	@echo built
	@echo "print-flags=[$(MAKEFLAGS)]"

.IGNORE: ignored
.SILENT: quiet

ignored:
	false
	@echo ignored-continued

quiet:
	echo quiet-result

loud:
	echo loud-result

broken:
	false
EOF
"$BIN" -c 'koshkit make -p print' > database.out
grep -c '^VALUE = printed$' database.out
grep -c '^print:$' database.out
grep -c '^print-flags=\[\]$' database.out
: > empty.mk
"$BIN" -c 'koshkit make -p -f empty.mk' > empty-database.out 2> "$TEST_NULL_DEVICE"
echo "empty-database=$?"
"$BIN" -c 'koshkit make ignored'
"$BIN" -c 'koshkit make quiet'
"$BIN" -c 'koshkit make loud'
"$BIN" -c 'koshkit make broken' 2> "$TEST_NULL_DEVICE"
echo "target-ignore=$?"
printf 'stdin-target: ; @echo stdin-makefile\n' |
  "$BIN" -c 'koshkit make -f - stdin-target'

echo "--- inherited flags and forced dry-run recipes ---"
cat > Makefile <<'EOF'
inherited:
	@echo inherited-ran > inherited-marker

forced:
	+@echo forced-ran > forced-marker

ordinary:
	@echo ordinary-ran > ordinary-marker
EOF
MAKEFLAGS=n "$BIN" -c 'koshkit make inherited'
[ -e inherited-marker ] && echo inherited-dry=no || echo inherited-dry=yes
"$BIN" -c 'koshkit make -n forced ordinary'
[ -e forced-marker ] && echo forced-ran=yes || echo forced-ran=no
[ -e ordinary-marker ] && echo ordinary-ran=yes || echo ordinary-ran=no
cat > recursive.mk <<'EOF'
all:
	@$(MAKE) -f recursive-child.mk show
EOF
cat > recursive-child.mk <<'EOF'
show:
	@echo "recursive-value=[$(VALUE)]"
EOF
"$BIN" -c 'koshkit make -f recursive.mk "VALUE=one two"'
cat > inherited-assignment.mk <<'EOF'
show:
	@echo "inherited-macro=$(HIDDEN) inherited-environment=$${HIDDEN-unset}"
EOF
unset HIDDEN
MAKEFLAGS=HIDDEN=fromflags \
  "$BIN" -c 'koshkit make -f inherited-assignment.mk show'

echo "--- forced query and touch recipes ---"
cat > Makefile <<'EOF'
forced-query:
	+@echo query-ran > query-marker

forced-touch:
	+@echo touch-ran > touch-marker

ordinary-touch:
	@echo ordinary-touch-ran > ordinary-touch-marker
EOF
"$BIN" -c 'koshkit make -q forced-query'
echo "query-status=$?"
[ -e query-marker ] && echo query-ran=yes || echo query-ran=no
"$BIN" -c 'koshkit make -t forced-touch ordinary-touch'
[ -e touch-marker ] && echo touch-ran=yes || echo touch-ran=no
[ -e ordinary-touch-marker ] && echo ordinary-touch-ran=yes || echo ordinary-touch-ran=no
[ -e forced-touch ] && echo forced-touched=yes || echo forced-touched=no
[ -e ordinary-touch ] && echo ordinary-touched=yes || echo ordinary-touched=no

echo "--- POSIX automatic macro forms ---"
mkdir -p dir pkg obj
: > dir/input.c
: > newer.h
cat > Makefile <<'EOF'
.SUFFIXES: .c .o

dir/output.o: dir/input.c newer.h
	@echo "target=$@ dir=$(@D) file=$(@F)"
	@echo "first=$< dir=$(<D) file=$(<F)"
	@echo "newer=$? dirs=$(?D) files=$(?F) stem=$*"

pkg/lib.a(obj/member.o):
	@echo "archive=$@ member=$% archive-dir=$(@D) archive-file=$(@F)"
	@echo "member-dir=$(%D) member-file=$(%F)"
EOF
"$BIN" -c 'koshkit make dir/output.o'
"$BIN" -c 'koshkit make "pkg/lib.a(obj/member.o)"'

echo "--- POSIX recipe environment and shell macro ---"
cat > Makefile <<'EOF'
INHERITED = fromfile
LOCAL = fromfile

all:
	@echo "command=$${COMMAND} inherited=$${INHERITED} local=$${LOCAL}"
EOF
INHERITED=fromenvironment \
  "$BIN" -c 'koshkit make COMMAND=fromcommand'
cat > shell.mk <<EOF
SHELL = $TEST_SYSTEM_PATH/echo

all:
	@echo shell-selected
EOF
shell_result=$("$BIN" -c 'koshkit make -f shell.mk')
case "$shell_result" in
  (*'-c echo shell-selected') echo shell-selected=yes ;;
  (*) echo "shell-selected=no [$shell_result]" ;;
esac
cat > shell-env.mk <<'EOF'
SHELL = /bin/sh

all:
	@echo "shell-env=$$SHELL"
EOF
SHELL=inherited-value "$BIN" -c 'koshkit make -f shell-env.mk'

echo "--- POSIX graph and inference edge cases ---"
cat > Makefile <<'EOF'
all: failed-prerequisite independent-prerequisite

failed-prerequisite:
	@false

independent-prerequisite:
	@echo nested-independent
EOF
"$BIN" -c 'koshkit make -k all' 2> "$TEST_NULL_DEVICE"
echo "nested-keep=$?"
cat > Makefile <<'EOF'
.SUFFIXES:
.SUFFIXES: .src
.SUFFIXES: .out

.src:
	@echo "single-source=$< single-target=$@"

.src.out:
	@echo "double-source=$< double-target=$@ prerequisites=$^"

tool:

item.out: extra
EOF
: > tool.src
: > item.src
: > extra
"$BIN" -c 'koshkit make tool'
"$BIN" -c 'koshkit make item.out'

echo "--- POSIX macro and built-in database ---"
cat > Makefile <<'EOF'
A = first
NAME = A
$(NAME) = expanded-name

all: duplicate
	@echo "paren=$(A) brace=${A} short=$A shell=$(SHELL)"

duplicate: first second first
	@echo "caret=$^"
	@echo "plus=$+"

first second:
	@:
EOF
"$BIN" -c 'koshkit make'
cat > makeflags.mk <<'EOF'
all:
	@case "$(MAKEFLAGS)" in (*'NAME=value'*) echo command-flags=yes;; (*) echo command-flags=no;; esac
EOF
"$BIN" -c 'koshkit make -s -f makeflags.mk NAME=value'
"$BIN" -c 'koshkit make -p -f Makefile all' > builtins.out
grep '^\.SUFFIXES:' builtins.out
grep '^CC = c99$' builtins.out
grep '^CFLAGS = -O 1$' builtins.out
grep '^\.c\.a:$' builtins.out
grep '^\.sh:$' builtins.out
grep '^\.y\.o:$' builtins.out
grep '^\.l\.o:$' builtins.out
grep '^\.y\.c:$' builtins.out
grep '^\.l\.c:$' builtins.out
grep -c '^\.f:$' builtins.out
grep -c '^\.f\.o:$' builtins.out
grep -c '^\.f\.a:$' builtins.out
"$BIN" -c 'koshkit make -r -p -f Makefile all' > no-builtins.out
grep '^\.SUFFIXES:$' no-builtins.out
cat > Makefile <<'EOF'
.POSIX:
CC = echo
CFLAGS =
LDFLAGS =
EOF
: > program.c
"$BIN" -c 'koshkit make program'

echo "--- POSIX includes and recipe continuation ---"
cat > nested.mk <<'EOF'
NESTED = nested
EOF
cat > included.mk <<'EOF'
include nested.mk
INCLUDED = included

included-target:
	@echo "include=$(INCLUDED) $(NESTED)"
EOF
cat > Makefile <<'EOF'
INCLUDE_FILE = included.mk
include $(INCLUDE_FILE)

all: included-target continued

continued:
	@printf '<%s>\n' 'left\
	right'
EOF
"$BIN" -c 'koshkit make all'

echo "--- POSIX archive inference ---"
cat > Makefile <<'EOF'
.SUFFIXES:
.SUFFIXES: .o .src .a

.src.a:
	@echo "archive=$@ member=$% source=$< stem=$*"
EOF
: > member.src
"$BIN" -c 'koshkit make "library.a(member.o)"'
printf '!<arch>\n%-16s%-12s%-6s%-6s%-8s%-10s`\n' \
  'member.o/' '1577836800' '0' '0' '100644' '0' > timed.a
: > timed.src
touch -t 202101010000 timed.src
touch -t 202201010000 timed.a
cat > Makefile <<'EOF'
timed.a(member.o): timed.src
	@echo archive-member-stale
EOF
"$BIN" -c 'koshkit make "timed.a(member.o)"'

echo "--- POSIX suffix precedence ---"
cat > Makefile <<'EOF'
.SUFFIXES:
.SUFFIXES: .sh .c

.c:
	@echo via-c

.sh:
	@echo via-sh
EOF
: > ordered.c
: > ordered.sh
"$BIN" -c 'koshkit make ordered'
cat > Makefile <<'EOF'
.SUFFIXES:
.SUFFIXES: .src .out

.src.out:
	@echo first-inference

.src.out:
	@echo second-inference
EOF
: > replaced.src
"$BIN" -c 'koshkit make replaced.out'
cat > Makefile <<'EOF'
.config:
	@echo dot-default
EOF
"$BIN" -c 'koshkit make'

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
