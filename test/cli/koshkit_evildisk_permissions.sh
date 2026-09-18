#!/bin/sh

output=$($BIN -c 'koshkit evildisk' 2>&1 >/dev/null)
case "$output" in
  '') classification=checked ;;
  warning:\ skipped\ *\ filesystem\ due\ to\ permission\ denied.) classification=checked ;;
  warning:\ skipped\ *\ filesystems\ due\ to\ permission\ denied.) classification=checked ;;
  *) classification=unexpected ;;
esac
printf 'permission-classification=%s\n' "$classification"
