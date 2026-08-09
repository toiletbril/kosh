unset SHIT_FLAGS
# A -WWW unset-variable warning carets the statement that reads the variable,
# so a read inside [[ ]], a case subject, (( )), a for word list, or an array
# literal points at its own line rather than the statement before it.
script=$(mktemp)
cat > "$script" <<'EOF'
previous_statement=1
[[ x = "$UNSET_COND" ]]
previous_statement=2
case $UNSET_SUBJECT in *) : ;; esac
previous_statement=3
(( UNSET_ARITH + 1 ))
previous_statement=4
for f in $UNSET_LIST; do :; done
previous_statement=5
a=( $UNSET_ELEM )
EOF
"$BIN" -WWW "$script" 2>&1 | grep -E 'warning' | sed "s|$script|SCRIPT|"
rm -f "$script"
echo "rc=$?"
