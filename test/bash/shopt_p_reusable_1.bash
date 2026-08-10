#!/bin/bash
# Alternative for shopt_p_reusable.bash on a bash whose bare `shopt name` pads
# the option column to a width other than kosh's fixed 20 characters. The bare
# shopt line is spelled with that width here, while the reusable shopt -p forms
# match across bash versions and stay as they are.
shopt -p globstar
shopt -p extglob nullglob
shopt -po pipefail
reset=$(shopt -p globstar)
shopt -s globstar
$reset
printf '%-20s\toff\n' globstar
shopt -p globstar
