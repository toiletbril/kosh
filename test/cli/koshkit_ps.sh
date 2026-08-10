# The ps listing names live processes, so the rows differ on every machine and
# only the shape is asserted. The plain header is fixed, and the aux header and
# the presence of the running shell are matched rather than printed in full.
unset KOSH_FLAGS

echo "--- plain ps header ---"
"$BIN" -c 'koshkit ps' | head -1

echo "--- aux header has the USER and COMMAND columns ---"
if "$BIN" -c 'koshkit ps aux' | head -1 | grep -qE '^USER +PID +VSZ +RSS STAT COMMAND$'; then
    echo "header ok"
else
    echo "header bad"
fi

echo "--- aux lists the running shell ---"
if "$BIN" -c 'koshkit ps aux' | grep -q 'kosh'; then
    echo "self ok"
else
    echo "self missing"
fi

echo "--- -aux selects the same wide listing ---"
if "$BIN" -c 'koshkit ps -aux' | head -1 | grep -qE '^USER +PID +VSZ +RSS STAT COMMAND$'; then
    echo "dash aux ok"
else
    echo "dash aux bad"
fi
