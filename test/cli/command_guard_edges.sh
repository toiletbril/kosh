#!/bin/bash

run_probe()
{
  "$BIN" -c "$2" >/dev/null 2>&1
  printf '%s=%s\n' "$1" "$?"
}

run_probe sequence 'command -v guard_missing; guard_missing'
run_probe negated_and '! command -v guard_missing && guard_missing'
run_probe negated_if 'if ! command -v guard_missing; then guard_missing; fi'
run_probe shadowed_command 'command(){ false; }; command -v guard_missing && guard_missing'
run_probe shadowed_type 'type(){ false; }; type guard_missing && guard_missing'
run_probe shadowed_hash 'hash(){ false; }; hash guard_missing && guard_missing'
run_probe until_failure 'until command -v guard_missing; do guard_missing; break; done'
run_probe valid_or '! command -v printf || printf okay'
run_probe valid_until 'until ! command -v printf; do printf okay; break; done'
run_probe zsh_guard 'if test -n "${ZSH_VERSION+set}" && (emulate sh) >/dev/null 2>&1; then emulate sh; setopt NO_GLOB_SUBST; zmodload zsh/parameter; fi'
run_probe zsh_semicolon 'test -n "${ZSH_VERSION+set}"; emulate sh'
run_probe zsh_else 'if test -n "${ZSH_VERSION+set}"; then :; else emulate sh; fi'
run_probe zsh_negated 'if ! test -n "${ZSH_VERSION+set}"; then setopt NO_GLOB_SUBST; fi'
run_probe zsh_other_variable 'if test -n "${BASH_VERSION+set}"; then zmodload zsh/parameter; fi'
run_probe zsh_shadowed_test 'test(){ true; }; if test -n "${ZSH_VERSION+set}"; then emulate sh; fi'
