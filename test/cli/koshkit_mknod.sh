#!/bin/sh

root=$TEST_TEMP_DIRECTORY/mknod
mkdir -p "$root"

"$BIN" -c 'koshkit mknod "$1/fifo-posix" p' mknod "$root"
posix_status=$?
if test -p "$root/fifo-posix"; then posix_type=fifo; else posix_type=missing; fi
printf 'posix-status=%s type=%s\n' "$posix_status" "$posix_type"

"$BIN" -c 'koshkit mknod --fifo --mode 600 "$1/fifo-modern"' \
  mknod "$root"
modern_status=$?
if test -p "$root/fifo-modern"; then modern_type=fifo; else modern_type=missing; fi
printf 'modern-status=%s type=%s\n' "$modern_status" "$modern_type"

"$BIN" -c 'koshkit mknod --mode definitely-not-a-mode "$1/bad"' \
  mknod "$root" >/dev/null 2>&1
printf 'invalid-mode-status=%s\n' "$?"

rm -f "$root/fifo-posix" "$root/fifo-modern"
if test ! -e "$root/fifo-posix" && test ! -e "$root/fifo-modern"; then
  cleanup=ok
else
  cleanup=failed
fi
printf 'cleanup=%s\n' "$cleanup"
