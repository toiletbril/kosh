unset SHIT_FLAGS
# Unset of an array element is not glob-expanded, so unset a[1] under failglob
# removes the element rather than treating the subscript as a glob.
echo "== unset of an array element is not glob-expanded under failglob:"
"$BIN" -c 'a=(x y z); unset '\''a[1]'\''; echo "${a[@]}"'
