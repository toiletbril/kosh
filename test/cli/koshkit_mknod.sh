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
modern_mode=$(stat -c '%a' "$root/fifo-modern" 2>/dev/null || printf missing)
printf 'modern-status=%s type=%s mode=%s\n' "$modern_status" "$modern_type" "$modern_mode"

"$BIN" -c 'koshkit mknod --mode definitely-not-a-mode "$1/bad"' \
  mknod "$root" >/dev/null 2>&1
printf 'invalid-mode-status=%s\n' "$?"

"$BIN" -c 'koshkit mknod --character --major 4096 --minor 0 "$1/device"' \
  mknod "$root" >/dev/null 2>&1
printf 'invalid-device-status=%s\n' "$?"

missing_major_error=$($BIN -c 'koshkit mknod --block "$1/missing-major"' \
  mknod "$root" 2>&1 >/dev/null)
missing_major_status=$?
case "$missing_major_error" in
  *"Missing major device number"*) missing_major_check=ok ;;
  *) missing_major_check=missing ;;
esac
printf 'missing-major-status=%s check=%s\n' "$missing_major_status" \
  "$missing_major_check"

missing_minor_error=$($BIN -c \
  'koshkit mknod --block --major 1 "$1/missing-minor"' \
  mknod "$root" 2>&1 >/dev/null)
missing_minor_status=$?
case "$missing_minor_error" in
  *"Missing minor device number"*) missing_minor_check=ok ;;
  *) missing_minor_check=missing ;;
esac
printf 'missing-minor-status=%s check=%s\n' "$missing_minor_status" \
  "$missing_minor_check"

device_path=$root/device
device_error=$($BIN -c 'koshkit mknod --character --major 1 --minor 3 "$1"' \
  mknod "$device_path" 2>&1)
device_status=$?
if test -c "$device_path"; then
  device_check=ok
  rm -f "$device_path"
elif test "$device_status" -ne 0; then
  case "$device_error" in
    *"permission denied"*|*"Operation not permitted"*) device_check=ok ;;
    *) device_check=failed ;;
  esac
else
  device_check=failed
fi
printf 'device-check=%s\n' "$device_check"

help=$($BIN -c 'koshkit mknod --help')
case "$help" in
  *"mknod --fifo pipe"*"mknod --character --major 1 --minor 3 device"*)
    help_examples=present ;;
  *) help_examples=missing ;;
esac
printf 'help-examples=%s\n' "$help_examples"

error=$($BIN -c 'koshkit mknod --fifo --mode invalid "$1/bad"' mknod "$root" 2>&1 >/dev/null)
case "$error" in
  *"error:"*"Invalid mode"*) redirected_error=plain ;;
  *) redirected_error=missing ;;
esac
printf 'redirected-error=%s\n' "$redirected_error"

"$BIN" -c 'koshkit mknod --fifo "$1/fifo-extra" p' mknod "$root" \
  >/dev/null 2>&1
printf 'named-extra-status=%s\n' "$?"

rm -f "$root/fifo-posix" "$root/fifo-modern"
if test ! -e "$root/fifo-posix" && test ! -e "$root/fifo-modern"; then
  cleanup=ok
else
  cleanup=failed
fi
printf 'cleanup=%s\n' "$cleanup"
