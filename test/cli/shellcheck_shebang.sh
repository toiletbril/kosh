unset KOSH_FLAGS
start=$PWD
dir=$(mktemp -d)
trap 'cd "$start" && rm -rf "$dir"' EXIT
cd "$dir"

run_case() {
  name=$1
  printf '%s' "$2" > "$name.kosh"
  echo "== $name =="
  "$BIN" -n -WWW "$name.kosh" 2>&1
}

run_case unknown_interpreter '#!/bin/perl
echo hi
'
run_case swapped '!#/bin/sh
echo hi
'
run_case missing_hash '!/bin/sh
echo hi
'
run_case missing_bang '# /bin/sh
echo hi
'
run_case leading_space '  #!/bin/sh
echo hi
'
run_case inner_space '# !/bin/sh
echo hi
'
run_case not_first_line '# Copyright 2018
#!/bin/sh
echo hi
'
run_case parameter_count '#!/bin/sh -e -u
echo hi
'
run_case missing_shebang 'echo hi
'
run_case relative_interpreter '#!bin/sh
echo hi
'
run_case directory_interpreter '#!/bin/sh/
echo hi
'
run_case env_split_string '#!/usr/bin/env -S bash
echo hi
'
run_case plain_sh '#!/bin/sh
echo hi
'
run_case leading_negation '#!/usr/bin/env kosh
! true
'
run_case ordinary_comment_header '# Copyright 2018
echo hi
'

echo "== stdin carries no shebang =="
printf 'echo hi\n' | "$BIN" -n -WWW - 2>&1
echo "rc=$?"
