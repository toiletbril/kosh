#!/bin/sh
# Normalize the CLI invocation trace so a golden does not break when the
# binary moves between rel, dbg, or a different build tree. The trace echoes
# argv[0], and the Makefile resolves BIN to an absolute path, so the path
# leaks into the output. The path is replaced by the stable token KOSH, the
# trace column is zeroed since the caret width depends on the real path
# length, and the caret line that follows the KOSH invocation is dropped,
# the way warning_source_chain normalizes INNER and OUTER.
#
# A trace caret spans the real source operand, so its width also follows the
# checkout path length. Every caret under a trace line collapses to ^~~~ and
# the golden stays the same on any checkout. A warning caret is left alone,
# because it spans the reported word and its width is the finding.
#
# Usage: ... | normalize-trace.sh "$BIN"
BIN=$1
BIN_FORWARD=$(printf '%s\n' "$BIN" | tr '\\' '/')
BIN_BACKWARD=$(printf '%s\n' "$BIN_FORWARD" | tr '/' '\\')
BIN_PATTERN=$(printf '%s\n' "$BIN" | sed 's/[][\\.^$*]/\\&/g; s/#/\\#/g')
BIN_FORWARD_PATTERN=$(printf '%s\n' "$BIN_FORWARD" | sed 's/[][\\.^$*]/\\&/g; s/#/\\#/g')
BIN_BACKWARD_PATTERN=$(printf '%s\n' "$BIN_BACKWARD" | sed 's/[][\\.^$*]/\\&/g; s/#/\\#/g')
sed "s#$BIN_PATTERN#KOSH#g; s#$BIN_FORWARD_PATTERN#KOSH#g; s#$BIN_BACKWARD_PATTERN#KOSH#g; s#'KOSH'#KOSH#g; s/^\([0-9][0-9]*\):[0-9][0-9]*: trace:/\1:0: trace:/" |
  sed '/: trace:$/ {n; n; s/\^~*/^~~~/;}' |
  sed '/KOSH.\{0,1\} -/{n; d;}'
