unset KOSH_FLAGS
# A set --init-moods inside ~/.koshrc must not recurse into the same flavor and
# overflow the stack, and the nested bash source must reuse the live arena rather
# than reset it under the tree being evaluated, which would dangle that tree's
# nodes. The shell sources the rc once, then exits because the /dev/null stdin is
# not a terminal. The exit status is what proves there was no crash, since a
# stack overflow or a corrupted arena raises a signal and exits 139 or 134
# rather than the clean non-terminal status. The ~/.bashrc carries an array
# assignment, the construct that tripped the arena corruption.
home=$(mktemp -d)
trap 'rm -rf "$home"' EXIT
printf 'echo RC-MARK\nset --init-moods kosh,bash\n' > "$home/.koshrc"
printf 'arr=(a b c)\necho "bashrc-arr=${arr[1]}"\n' > "$home/.bashrc"
HOME="$home" "$BIN" -i </dev/null >"$home/out" 2>&1
case "$?" in
	139 | 134) echo "exit=crash" ;;
	*) echo "exit=clean" ;;
esac
printf 'rc-mark=%s\n' "$(grep -c '^RC-MARK$' "$home/out")"
