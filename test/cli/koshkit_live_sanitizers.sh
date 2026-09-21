#!/bin/sh

# Run short live-mode probes against the sanitizer-enabled debug binary.
set -eu

: "${BIN:?BIN must point to the sanitizer-enabled kosh binary}"

if [ "${TARGET:-$(uname -s)}" != Linux ]; then
  echo "sanitizer live probes: skipped (requires Linux /proc)"
  exit 0
fi

run_probe() {
  name=$1
  command=$2
  output=$(mktemp "${TMPDIR:-/tmp}/kosh-${name}.XXXXXX")
  errors=$(mktemp "${TMPDIR:-/tmp}/kosh-${name}-err.XXXXXX")
  trap 'rm -f "$output" "$errors"' EXIT HUP INT TERM

  set -m
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
    "$BIN" -Q -c "$command" >"$output" 2>"$errors" &
  pid=$!
  set +m
  (
    sleep 4
    if kill -0 "$pid" 2>"$TEST_NULL_DEVICE"; then
      kill -KILL "$pid" 2>"$TEST_NULL_DEVICE" || true
    fi
  ) &
  watchdog=$!
  max_rss=0
  attempt=0
  while [ "$attempt" -lt 30 ] && kill -0 "$pid" 2>"$TEST_NULL_DEVICE"; do
    rss=$(awk '/VmRSS:/ {print $2}' "/proc/$pid/status" 2>"$TEST_NULL_DEVICE" || true)
    case $rss in
      ''|*[!0-9]*) ;;
      *) [ "$rss" -gt "$max_rss" ] && max_rss=$rss ;;
    esac
    attempt=$((attempt + 1))
    sleep 0.05
  done
  kill -INT "$pid" 2>"$TEST_NULL_DEVICE" || true
  set +e
  wait "$pid"
  status=$?
  set -e
  kill "$watchdog" 2>"$TEST_NULL_DEVICE" || true
  wait "$watchdog" 2>"$TEST_NULL_DEVICE" || true

  if grep -Eiq 'addresssanitizer|undefinedbehavior|runtime error|leak' \
    "$errors"
  then
    diagnostics=found
  else
    diagnostics=none
  fi
  printf '%s status=%s max_rss_kib=%s diagnostics=%s\n' \
    "$name" "$status" "$max_rss" "$diagnostics"
  [ "$status" -eq 130 ] && [ "$diagnostics" = none ]
  rm -f "$output" "$errors"
  trap - EXIT HUP INT TERM
}

run_probe evilio-process \
  'koshkit --color never evilio --ps --live=0.02 --cumulative=0.05 -5'
run_probe evilio-disk \
  'koshkit --color never evilio --live=0.02 --cumulative=0.05'
run_probe evilnet \
  'koshkit --color never evilnet --traffic --live=0.02 --cumulative=0.05'
run_probe evilps \
  'koshkit --color never evilps --cpu --live=0.02 --cumulative=0.05 -5'
