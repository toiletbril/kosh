#!/bin/bash
history -c
history -s one alpha
history -s two
history -s three
history
history -d 2
history
history -d -1
history
history -s two
history -s three
history -d 1-2
history
history -d 9
printf 'invalid=%s\n' "$?"
history
history -c
history -s one
history -s two
history -s three
history -d 3-2
printf 'reversed=%s\n' "$?"
history -d -3--2
history
