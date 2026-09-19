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

invalid_major_error=$($BIN -c \
  'koshkit mknod --block --major not-a-major --minor 2 "$1/invalid-major"' \
  mknod "$root" 2>&1 >/dev/null)
invalid_major_status=$?
case "$invalid_major_error" in
  *"Invalid major device number 'not-a-major'"*) invalid_major_check=ok ;;
  *) invalid_major_check=missing ;;
esac
printf 'invalid-major-status=%s check=%s\n' "$invalid_major_status" \
  "$invalid_major_check"

invalid_minor_error=$($BIN -c \
  'koshkit mknod "$1/invalid-minor" b 1 not-a-minor' \
  mknod "$root" 2>&1 >/dev/null)
invalid_minor_status=$?
case "$invalid_minor_error" in
  *"Invalid minor device number 'not-a-minor'"*) invalid_minor_check=ok ;;
  *) invalid_minor_check=missing ;;
esac
printf 'invalid-minor-status=%s check=%s\n' "$invalid_minor_status" \
  "$invalid_minor_check"

masked_major_error=$($BIN -c \
  'koshkit mknod --block --major not-a-major "$1/missing-minor"' \
  mknod "$root" 2>&1 >/dev/null)
case "$masked_major_error" in
  *"Invalid major device number 'not-a-major'"*) masked_major_check=ok ;;
  *) masked_major_check=missing ;;
esac
printf 'masked-major-check=%s\n' "$masked_major_check"

fifo_number_error=$($BIN -c \
  'koshkit mknod --fifo --major not-a-major "$1/fifo-number"' \
  mknod "$root" 2>&1 >/dev/null)
case "$fifo_number_error" in
  *"Device numbers require a character or block node"*)
    fifo_number_check=ok ;;
  *) fifo_number_check=missing ;;
esac
printf 'fifo-number-check=%s\n' "$fifo_number_check"

hybrid_named_error=$($BIN -c \
  'koshkit mknod --block "$1/hybrid-named" hybrid-major 2' \
  mknod "$root" 2>&1 >/dev/null)
case "$hybrid_named_error" in
  *"Invalid major device number 'hybrid-major'"*) hybrid_named_check=ok ;;
  *) hybrid_named_check=missing ;;
esac
printf 'hybrid-named-check=%s\n' "$hybrid_named_check"

hybrid_major_error=$($BIN -c \
  'koshkit mknod --major 1 "$1/hybrid-major" b hybrid-minor' \
  mknod "$root" 2>&1 >/dev/null)
case "$hybrid_major_error" in
  *"Invalid minor device number 'hybrid-minor'"*) hybrid_major_check=ok ;;
  *) hybrid_major_check=missing ;;
esac
printf 'hybrid-major-check=%s\n' "$hybrid_major_check"

hybrid_minor_error=$($BIN -c \
  'koshkit mknod --minor 2 "$1/hybrid-minor" b hybrid-major' \
  mknod "$root" 2>&1 >/dev/null)
case "$hybrid_minor_error" in
  *"Invalid major device number 'hybrid-major'"*) hybrid_minor_check=ok ;;
  *) hybrid_minor_check=missing ;;
esac
printf 'hybrid-minor-check=%s\n' "$hybrid_minor_check"

unused_error=$($BIN -c \
  'koshkit mknod --major 1 --minor 2 --block 1 "$1/unused"' \
  mknod "$root" 2>&1 >/dev/null)
case "$unused_error" in
  *"Too many operands"*) unused_check=ok ;;
  *) unused_check=missing ;;
esac
printf 'unused-operand-check=%s\n' "$unused_check"

posix_device_path=$root/device-posix
posix_device_error=$($BIN -c 'koshkit mknod "$1" c 1 3' \
  mknod "$posix_device_path" 2>&1)
posix_device_status=$?
if test -c "$posix_device_path"; then
  posix_device_numbers=$(stat -c '%t:%T' "$posix_device_path")
  if test "$posix_device_numbers" = 1:3; then
    posix_device_check=ok
  else
    posix_device_check=failed
  fi
  "$TEST_SYSTEM_RM" -f "$posix_device_path"
elif test "$posix_device_status" -ne 0; then
  case "$posix_device_error" in
    *"permission denied"*|*"Operation not permitted"*|*"not supported"*)
      posix_device_check=ok ;;
    *) posix_device_check=failed ;;
  esac
else
  posix_device_check=failed
fi
printf 'posix-device-check=%s\n' "$posix_device_check"

device_path=$root/device
device_error=$($BIN -c 'koshkit mknod --character --major 1 --minor 3 "$1"' \
  mknod "$device_path" 2>&1)
device_status=$?
if test -c "$device_path"; then
  device_numbers=$(stat -c '%t:%T' "$device_path")
  if test "$device_numbers" = 1:3; then
    device_check=ok
  else
    device_check=failed
  fi
  "$TEST_SYSTEM_RM" -f "$device_path"
elif test "$device_status" -ne 0; then
  case "$device_error" in
    *"permission denied"*|*"Operation not permitted"*|*"not supported"*)
      device_check=ok ;;
    *) device_check=failed ;;
  esac
else
  device_check=failed
fi
printf 'device-check=%s\n' "$device_check"

type_device_error=$($BIN -c \
  'koshkit mknod --major 1 --type=character --minor 3 "$1"' \
  mknod "$root/device-type" 2>&1)
case "$type_device_error" in
  *"Cannot create '$root/device-type'"*) type_device_check=ok ;;
  *)
    if test -c "$root/device-type"; then
      type_device_check=ok
      "$TEST_SYSTEM_RM" -f "$root/device-type"
    else
      type_device_check=failed
    fi
    ;;
esac
printf 'type-device-check=%s\n' "$type_device_check"

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
