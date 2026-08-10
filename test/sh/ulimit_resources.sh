#!/bin/sh
# ulimit reads resource limits and the -a table, checked against dash. The
# values come from the same kernel, so kosh and dash report the same numbers.
# The -u flag is a bash-style alias the test avoids, since dash has none.

# The core limit is pinned to a known value first, since kosh drops it to zero at
# startup as a courtesy while dash inherits whatever the environment carried, so
# without pinning the coredump line would differ by environment rather than by a
# real shell difference.
ulimit -c 0

ulimit -a
ulimit -n
ulimit -s
ulimit -c
ulimit -d
ulimit -v
ulimit -H -n
ulimit -S -n

# A soft-limit set is visible to a later read in the same subshell.
( ulimit -S -n 256; ulimit -n )
