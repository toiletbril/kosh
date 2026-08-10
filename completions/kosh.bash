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
  esac

  local long_flags="--version --short-version --help --interactive --stdin \
--command --error-exit --no-glob --one-command --verbose --xtrace --export-all \
--no-clobber --no-exec --no-unset --login --rcfile --init-file --norc \
--restricted --privileged --clean --posix --mood \
--init-moods --mimicry --dumb --list-diagnostics \
--no-diagnostics --no-annoying-diagnostics --no-init-diagnostics --no-traces --no-completion --no-syntax-highlighting \
--enable-koshkit \
--show-ast \
--show-optimizer-state --show-exit-code --show-lexed-words --show-stats --show-memory \
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

_koshkit_utils="basename calc cat cp dirname du env find flock grep head killall ln \
ls make mkdir mv nproc pkill ps realpath rm rmdir seq sleep sort tail tee timeout touch tr \
uniq unlink wc which whoami yes"

_koshkit_util_flags()
{
  case $1 in
    ls)            echo "-a -1 -l -h" ;;
    nproc)         echo "--all --ignore=" ;;
    ln)            echo "-s -f" ;;
    rm)            echo "-r -R -f" ;;
    mkdir)         echo "-p" ;;
    cp)            echo "-r -R -v" ;;
    mv)            echo "-f -v" ;;
    cat)           echo "-n --syntax-highlighting" ;;
    tee)           echo "-a" ;;
    touch)         echo "-c" ;;
    du)            echo "-s -h" ;;
    head|tail)     echo "-n" ;;
    wc)            echo "-l -w -c" ;;
    tr)            echo "-d" ;;
    grep)          echo "-i -v" ;;
    sort)          echo "-r" ;;
    uniq)          echo "-c" ;;
    timeout)       echo "-s --signal -k --kill-after -p --preserve-status" ;;
    pkill|killall) echo "-s -l" ;;
    make)          echo "-f" ;;
    find)          echo "-name -type -maxdepth -mindepth -print" ;;
    flock)         echo "--transaction-held-lock" ;;
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
    _kosh_compgen -W "$(_koshkit_util_flags "$util")" -- "$current_word"
  else
    _kosh_compgen -f -- "$current_word"
  fi
}

complete -o filenames -F _koshkit_complete koshkit
