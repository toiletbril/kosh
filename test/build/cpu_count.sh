d=$(mktemp -d)
trap 'test -n "$d" && /bin/rm -rf "$d"' EXIT

detector=../scripts/cpu-count.sh

NUMBER_OF_PROCESSORS=12 PATH="$d" "$detector"

printf '#!/bin/sh\n[ "$1" = --all ] || exit 1\nprintf "6\\n"\n' > "$d/nproc"
chmod +x "$d/nproc"
PATH="$d" "$detector"

/bin/rm -f "$d/nproc"
printf '#!/bin/sh\nprintf "8\\n"\n' > "$d/sysctl"
chmod +x "$d/sysctl"
PATH="$d" "$detector"

/bin/rm -f "$d/sysctl"
printf '#!/bin/sh\nprintf "4\\n"\n' > "$d/getconf"
chmod +x "$d/getconf"
PATH="$d" "$detector"

/bin/rm -f "$d/getconf"
PATH="$d" "$detector"

default_jobs=$(cd .. && MAKEFLAGS= MFLAGS= MAKELEVEL=0 \
    make -n CPU_COUNT=6 kosh 2>/dev/null)
case "$default_jobs" in
    *'-j6 -C src kosh'*) echo default-make-uses-all ;;
    *) echo default-make-missed ;;
esac

variable_jobs=$(cd .. && MAKEFLAGS= MFLAGS= MAKELEVEL=0 \
    make -n CPU_COUNT=6 LABEL=project kosh 2>/dev/null)
case "$variable_jobs" in
    *'-j6 -C src kosh'*) echo variable-make-uses-all ;;
    *) echo variable-make-missed ;;
esac

explicit_jobs=$(cd .. && make -n -j2 CPU_COUNT=6 kosh 2>/dev/null)
case "$explicit_jobs" in
    *'-j6 -C src kosh'*) echo explicit-make-overridden ;;
    *) echo explicit-make-preserved ;;
esac

single_job=$(cd .. && make -n -j1 CPU_COUNT=6 kosh 2>/dev/null)
case "$single_job" in
    *'-j6 -C src kosh'*) echo single-make-overridden ;;
    *) echo single-make-preserved ;;
esac

single_job_without_discovery=$(cd .. && \
    make -n -j1 MAKE_COMMAND_LINE= CPU_COUNT=6 kosh 2>/dev/null)
case "$single_job_without_discovery" in
    *'-j6 -C src kosh'*) echo single-make-fallback-overridden ;;
    *) echo single-make-fallback-preserved ;;
esac

windows_jobs=$(cd .. && MAKEFLAGS= MFLAGS= MAKELEVEL=0 \
    OS=Windows_NT NUMBER_OF_PROCESSORS=7 \
    make -n kosh 2>/dev/null)
case "$windows_jobs" in
    *'-j7 -C src kosh'*) echo windows-make-uses-all ;;
    *) echo windows-make-missed ;;
esac

for invalid_count in 0 invalid; do
    windows_jobs=$(cd .. && MAKEFLAGS= MFLAGS= MAKELEVEL=0 OS=Windows_NT \
        NUMBER_OF_PROCESSORS=$invalid_count make -n kosh 2>/dev/null)
    case "$windows_jobs" in
        *"-j$invalid_count -C src kosh"*)
            echo windows-invalid-count-leaked
            exit 1
            ;;
    esac
done
echo windows-invalid-count-clean

if [ -e ../src/NUL ]; then
    echo windows-dry-run-leaked
else
    echo windows-dry-run-clean
fi

default_targets=$(cd .. && make -n MODE=dbg CPU_COUNT=1 2>/dev/null)
case "$default_targets" in
    *'Launching tests'*) echo default-make-launched-tests ;;
    *) echo default-make-builds-shell ;;
esac

commit_hash_dir=$d/commit-hash
commit_hash_log=$commit_hash_dir/compile.log
fake_cxx=$commit_hash_dir/fake-cxx
mkdir -p "$commit_hash_dir"
printf '%s\n' \
  '#!/bin/sh' \
  'output=' \
  'while [ "$#" -gt 0 ]; do' \
  '  if [ "$1" = -o ]; then' \
  '    shift' \
  '    output=$1' \
  '    break' \
  '  fi' \
  '  shift' \
  'done' \
  '[ -n "$output" ] || exit 0' \
  'printf "%s\n" "$output" >> "$FAKE_CXX_LOG"' \
  ': > "$output"' \
  > "$fake_cxx"
chmod +x "$fake_cxx"

commit_hash_cli_object=$commit_hash_dir/o/Cli.o
commit_hash_main_object=$commit_hash_dir/o/Main.o
commit_hash_control_object=$commit_hash_dir/o/Colors.o
FAKE_CXX_LOG=$commit_hash_log make -C ../src --no-print-directory \
  -j1 MODE=dbg OBJ_DIR="$commit_hash_dir/o" CXX="$fake_cxx" COMMIT_HASH=first \
  "$commit_hash_cli_object" "$commit_hash_main_object" \
  "$commit_hash_control_object" >/dev/null
printf 'commit-hash-first=%s\n' "$(wc -l < "$commit_hash_log" | tr -d ' ')"

FAKE_CXX_LOG=$commit_hash_log make -C ../src --no-print-directory \
  -j1 MODE=dbg OBJ_DIR="$commit_hash_dir/o" CXX="$fake_cxx" COMMIT_HASH=first \
  "$commit_hash_cli_object" "$commit_hash_main_object" \
  "$commit_hash_control_object" >/dev/null
printf 'commit-hash-same=%s\n' "$(wc -l < "$commit_hash_log" | tr -d ' ')"

FAKE_CXX_LOG=$commit_hash_log make -C ../src --no-print-directory \
  -j1 MODE=dbg OBJ_DIR="$commit_hash_dir/o" CXX="$fake_cxx" COMMIT_HASH=second \
  "$commit_hash_cli_object" "$commit_hash_main_object" \
  "$commit_hash_control_object" >/dev/null
printf 'commit-hash-changed=%s\n' "$(wc -l < "$commit_hash_log" | tr -d ' ')"
tail -2 "$commit_hash_log" | while IFS= read -r rebuilt_object; do
  printf 'commit-hash-rebuilt=%s\n' "${rebuilt_object##*/}"
done
