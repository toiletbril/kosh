#!/bin/bash
print_history_commands()
{
  history > "$TEST_TEMP_DIRECTORY/history-list"
  sed 's/^ *[0-9][0-9]*  //' "$TEST_TEMP_DIRECTORY/history-list"
}

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
history -c
HISTSIZE=3
history -s alpha
history -s beta
history -s gamma
HISTSIZE=1
printf 'shrink\n'
print_history_commands
HISTSIZE=3
printf 'grow\n'
print_history_commands
HISTSIZE=-1
history -s delta
printf 'negative\n'
print_history_commands
HISTSIZE=bad
history -s epsilon
printf 'invalid\n'
print_history_commands
