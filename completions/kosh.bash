#!/bin/bash
# bash completion for the kosh shell.
#
# Do:
# mv kosh.bash /usr/share/bash-completion/completions/kosh

_kosh_compgen()
{
  local candidate
  COMPREPLY=()
  while IFS= read -r candidate; do
    COMPREPLY+=("$candidate")
  done < <(compgen "$@")
}

_kosh_complete()
{
  local current_word previous_word
  current_word=${COMP_WORDS[COMP_CWORD]}
  previous_word=${COMP_WORDS[COMP_CWORD-1]}

  local mood_values="kosh bash sh bash-posix"
  case $previous_word in
    -M|--mood)
      _kosh_compgen -W "$mood_values" -- "$current_word"
      return
      ;;
    -L|--init-moods)
      _kosh_compgen -W "$mood_values" -- "$current_word"
      return
      ;;
    --rcfile|--init-file)
      _kosh_compgen -f -- "$current_word"
      return
      ;;
    -c|--command)
      COMPREPLY=()
      return
      ;;
  esac

  local long_flags="--version --short-version --help --interactive --stdin \
--command --error-exit --no-glob --one-command --verbose --xtrace --export-all \
--no-clobber --no-exec --no-unset --login --rcfile --init-file --norc \
--restricted --privileged --clean --posix --mood \
--init-moods --mimicry --dumb --lint --format --apply --language-server --list-diagnostics \
--no-diagnostics --no-annoying-diagnostics --no-init-diagnostics --no-traces --no-completion --no-syntax-highlighting \
--enable-koshkit \
--show-ast \
--optimizer-diagnostics --show-exit-code --show-lexed-words --show-stats --show-memory \
"

  local short_flags="-V -i -s -c -e -f -t -v -x -a -C -n -u -l -r -p -M -L -I -W -WW -WWW \
-T -A -E -R"

  if [[ $current_word == --* ]]; then
    _kosh_compgen -W "$long_flags" -- "$current_word"
  elif [[ $current_word == -* ]]; then
    _kosh_compgen -W "$short_flags $long_flags" -- "$current_word"
  else
    _kosh_compgen -f -- "$current_word"
  fi
}

complete -o filenames -F _kosh_complete kosh

_kosh_assimilate_complete()
{
  local current_word=${COMP_WORDS[COMP_CWORD]}
  local previous_word=${COMP_WORDS[COMP_CWORD - 1]}
  local flags="-x --trace --ssh-command --scp-command --link-mood --help"

  if [[ $previous_word == --link-mood ]]; then
    _kosh_compgen -W "bash dash sh kosh" -- "$current_word"
    return
  fi
  if [[ $current_word == -* ]]; then
    _kosh_compgen -W "$flags" -- "$current_word"
    return
  fi
  if declare -F _known_hosts_real >/dev/null; then
    _known_hosts_real "$current_word"
  fi
}

complete -F _kosh_assimilate_complete assimilate

_kosh_set_complete()
{
  local current_word=${COMP_WORDS[COMP_CWORD]}
  local previous_word=${COMP_WORDS[COMP_CWORD - 1]}
  local moods="kosh bash sh bash-posix"
  local option_names="allexport export-all notify errexit error-exit noglob no-glob \
hashall keyword monitor noexec no-exec nounset no-unset verbose xtrace braceexpand \
noclobber no-clobber errtrace physical functrace pipefail failglob koshkit vi emacs \
posix show-ast show-lexed-words show-exit-code mimicry annoying-diagnostics \
show-stats no-diagnostics show-memory login rcfile"
  local switches="--help --options --mood --init-moods -o +o -M -L \
-a -b -e -f -h -k -m -n -u -v -x -B -C -E -P -T -A -R -W -WW -WWW -I -S -G \
+a +b +e +f +h +k +m +n +u +v +x +B +C +E +P +T +A +R +W +WW +WWW +I +S +G"

  case $previous_word in
    -o|+o)
      _kosh_compgen -W "$option_names" -- "$current_word"
      return
      ;;
    -M|--mood)
      _kosh_compgen -W "$moods" -- "$current_word"
      return
      ;;
    -L|--init-moods)
      _kosh_compgen -W "$moods" -- "$current_word"
      return
      ;;
  esac

  _kosh_compgen -W "$switches" -- "$current_word"
}

complete -F _kosh_set_complete set

_kosh_fc_complete()
{
  local current_word=${COMP_WORDS[COMP_CWORD]}
  local previous_word=${COMP_WORDS[COMP_CWORD - 1]}
  local switches="--help -e -l -n -r -s"

  if [[ $previous_word == -e ]]; then
    _kosh_compgen -c -- "$current_word"
    return
  fi

  _kosh_compgen -W "$switches" -- "$current_word"
}

complete -F _kosh_fc_complete fc

_koshkit_utils="basename bc cal calc cat chgrp chmod chown cksum cmp comm cp csplit cut date \
df dirname du env expand expr file find flock fold getconf grep head id killall link ln locale \
logger logname ls make man mkdir mkfifo more mv nice nl nohup nproc od paste pathchk pkill pr ps \
realpath renice rm rmdir sed seq sleep sort split strings stty tabs tail tee timeout touch tput \
tr tsort tty uname unexpand uniq unlink uudecode uuencode wc which who whoami xargs yes"

_koshkit_util_flags()
{
  case $1 in
    bc)            echo "-l --mathlib -q --quiet" ;;
    calc)          echo "-i --interactive -p --pipe" ;;
    cp)            echo "-r -R -f -i -p -v" ;;
    cut)           echo "-b --bytes -c --characters -f --fields -d --delimiter -n --no-split -s --only-delimited" ;;
    file)          echo "-d --default-tests -h --no-dereference -i --regular-only -L --dereference -m --magic-file -M --magic-only" ;;
    ls)            echo "-a -A -1 -l -h" ;;
    nproc)         echo "--all --ignore=" ;;
    ln)            echo "-s -f -L -P" ;;
    locale)        echo "-a --all-locales -m --charmaps -c --category-name -k --keyword-name" ;;
    man)           echo "-k --keyword" ;;
    more)          echo "-c --clear -e --exit -i --ignore-case -s --squeeze -u --plain -n --lines -p --command -t --tag" ;;
    rm)            echo "-r -R -f -i --dry-run" ;;
    rmdir)         echo "-p" ;;
    mkdir)         echo "-p -m" ;;
    mv)            echo "-f -i -v" ;;
    od)            echo "-A --address-radix -j --skip-bytes -N --read-bytes -t --format -v --output-duplicates" ;;
    pr)            echo "-a --across -d --double-space -F -f -h --header -l --length -m --merge -n --number-lines -o --indent -r --no-file-warnings -t --omit-header -s --separator -w --width" ;;
    stty)          echo "-a --all -g --save" ;;
    cat)           echo "-n --syntax-highlighting" ;;
    tee)           echo "-a" ;;
    touch)         echo "-a -c -m -r -t" ;;
    tput)          echo "-T --terminal" ;;
    uudecode)      echo "-o --output" ;;
    who)           echo "-a --all -b --boot -d --dead -H --heading -l --login -m --current -p --process -q --quick -r --runlevel -s --short -t --time -T --terminal-state -u --idle" ;;
    du)            echo "-s -h" ;;
    head|tail)     echo "-n -c" ;;
    wc)            echo "-l -w -c" ;;
    tr)            echo "-d" ;;
    grep)          echo "-i -v" ;;
    sort)          echo "-r" ;;
    uniq)          echo "-c" ;;
    timeout)       echo "-s --signal -k --kill-after -p --preserve-status" ;;
    pkill|killall) echo "-s --signal -l --list" ;;
    make)          echo "-f --file -C --directory -B --always-make -k --keep-going -e --environment-overrides -i --ignore-errors -S --stop -n --just-print -p --print-data-base -q --question -r --no-builtin-rules -s --silent -t --touch" ;;
    find)          echo "-name -type -maxdepth -mindepth -print" ;;
    flock)         echo "--transaction-held-lock" ;;
    ps)            echo "-a -u -x -w" ;;
    which)         echo "-a --all -q --quiet" ;;
    *)             echo "" ;;
  esac
}

_koshkit_complete()
{
  local current_word
  current_word=${COMP_WORDS[COMP_CWORD]}

  if [[ $COMP_CWORD -eq 1 ]]; then
    _kosh_compgen -W "$_koshkit_utils --list --assimilate --help" \
      -- "$current_word"
    return
  fi

  local util=${COMP_WORDS[1]}
  if [[ $current_word == -* ]]; then
    _kosh_compgen -W "$(_koshkit_util_flags "$util") --help" -- "$current_word"
  else
    _kosh_compgen -f -- "$current_word"
  fi
}

complete -o filenames -F _koshkit_complete koshkit
