# The koshkit usage errors carry a how-to-fix note under the located message.
# Each utility is reached through `koshkit <name>` so a binary on PATH does not
# shadow the bundled one.
unset KOSH_FLAGS

echo "=== killall arg count ==="
"$BIN" -c 'koshkit killall a b' 2>&1

echo "=== seq zero increment ==="
"$BIN" -c 'koshkit seq 1 0 10' 2>&1

echo "=== seq integer location ==="
"$BIN" -c 'koshkit seq nope' 2>&1

echo "=== seq negative location preservation ==="
"$BIN" -c 'koshkit seq -2 nope' 2>&1

echo "=== seq extra operand location ==="
"$BIN" -c 'koshkit seq 1 2 3 4' 2>&1

echo "=== touch time location ==="
"$BIN" -c 'koshkit touch -t nope target' 2>&1

echo "=== touch time digit validation ==="
"$BIN" -c 'koshkit touch -t 2026aa010101 target' 2>&1

echo "=== touch calendar validation ==="
"$BIN" -c 'koshkit touch -t 202602300101 target' 2>&1

echo "=== touch seconds validation ==="
"$BIN" -c 'koshkit touch -t 200102030405.61 target' 2>&1

echo "=== touch later time location ==="
"$BIN" -c 'koshkit touch -r reference -t 200102030405.06 target' 2>&1

echo "=== touch later reference location ==="
"$BIN" -c 'koshkit touch -t 200102030405.06 -r reference target' 2>&1

echo "=== touch reference location ==="
"$BIN" -c 'koshkit touch -r KOSH_MISSING_REFERENCE target' 2>&1

echo "=== ln without -s ==="
"$BIN" -c 'koshkit ln a b' 2>&1

echo "=== tr one set ==="
"$BIN" -c 'koshkit tr abc' 2>&1

echo "=== find bad -type ==="
"$BIN" -c 'koshkit find . -type x' 2>&1

echo "=== readlink extra operand location ==="
"$BIN" -c 'koshkit readlink first second' 2>&1

echo "=== fuser conflicting flags location ==="
"$BIN" -c 'koshkit fuser -cf file' 2>&1
echo "=== whoami operand location ==="
"$BIN" -c 'koshkit whoami extra' 2>&1
echo "=== logname operand location ==="
"$BIN" -c 'koshkit logname extra' 2>&1

echo "=== nice increment location ==="
"$BIN" -c 'koshkit nice -n nope true' 2>&1
echo "=== nice increment range ==="
"$BIN" -c 'koshkit nice -n 2147483648 true' 2>&1
echo "=== nice command location ==="
"$BIN" -c 'koshkit nice KOSH_MISSING_COMMAND' 2>&1

echo "=== nl body style location ==="
"$BIN" -c 'koshkit nl -b invalid' 2>&1

echo "=== nl header style location ==="
"$BIN" -c 'koshkit nl -h invalid' 2>&1

echo "=== nl footer style location ==="
"$BIN" -c 'koshkit nl -f invalid' 2>&1

echo "=== nl number format location ==="
"$BIN" -c 'koshkit nl -n invalid' 2>&1

echo "=== nl increment location ==="
"$BIN" -c 'koshkit nl -i invalid' 2>&1

echo "=== nl increment limit ==="
"$BIN" -c 'koshkit nl -i 9223372036854775808' 2>&1

echo "=== nl blank group location ==="
"$BIN" -c 'koshkit nl -l 0' 2>&1

echo "=== nl width location ==="
"$BIN" -c 'koshkit nl -w 0' 2>&1

echo "=== nl start limit ==="
"$BIN" -c 'koshkit nl -v 9223372036854775808' 2>&1

echo "=== nl delimiter location ==="
"$BIN" -c 'koshkit nl -d :' 2>&1

echo "=== od address radix location ==="
"$BIN" -c 'koshkit od -A invalid' 2>&1

echo "=== od skip count location ==="
"$BIN" -c 'koshkit od -j invalid' 2>&1

echo "=== od byte count location ==="
"$BIN" -c 'koshkit od -N invalid' 2>&1

echo "=== od octal skip location ==="
"$BIN" -c 'koshkit od -j 08' 2>&1

echo "=== od hexadecimal byte count location ==="
"$BIN" -c 'koshkit od -N 0xg' 2>&1

echo "=== od legacy offset location ==="
"$BIN" -c 'koshkit od +8q' 2>&1

echo "=== od output type location ==="
"$BIN" -c 'koshkit od -t invalid' 2>&1

echo "=== od trailing output type location ==="
"$BIN" -c 'koshkit od -t x12' 2>&1

echo "=== od integer width location ==="
"$BIN" -c 'koshkit od -t x3' 2>&1

echo "=== od repeated output type location ==="
"$BIN" -c 'koshkit od -t x1 -t invalid' 2>&1

echo "=== pr page number location ==="
"$BIN" -c 'koshkit pr +0' 2>&1

echo "=== pr column count location ==="
"$BIN" -c 'koshkit pr -2x' 2>&1

echo "=== pr page length location ==="
"$BIN" -c 'koshkit pr -l invalid' 2>&1

echo "=== pr page width location ==="
"$BIN" -c 'koshkit pr -w invalid' 2>&1

echo "=== pr indentation location ==="
"$BIN" -c 'koshkit pr -o invalid' 2>&1

echo "=== pr header capacity location ==="
"$BIN" -c 'koshkit pr -l 9' 2>&1

echo "=== pr output capacity location ==="
"$BIN" -c 'koshkit pr -t -l 0' 2>&1

echo "=== pr column capacity location ==="
"$BIN" -c 'koshkit pr -t -l 2 -18446744073709551615' 2>&1

echo "=== grep pattern location ==="
"$BIN" -c "koshkit grep '['" 2>&1
echo "=== grep file location ==="
"$BIN" -c 'koshkit grep value KOSH_MISSING_FILE' 2>&1

echo "=== mv source location ==="
"$BIN" -c 'koshkit mv KOSH_MISSING_SOURCE KOSH_MISSING_DESTINATION' 2>&1

echo "=== renice identifier location ==="
"$BIN" -c 'koshkit renice -n 1 nope' 2>&1

echo "=== renice user location ==="
"$BIN" -c 'koshkit renice -n 1 -u KOSH_MISSING_USER' 2>&1

echo "=== renice increment location ==="
"$BIN" -c 'koshkit renice -n nope 1' 2>&1

echo "=== renice increment range ==="
"$BIN" -c 'koshkit renice -n 2147483648 1' 2>&1

echo "=== paste delimiter location ==="
"$BIN" -c "koshkit paste -d ''" 2>&1

echo "=== split suffix location ==="
"$BIN" -c 'koshkit split -a nope' 2>&1

echo "=== split suffix limit ==="
"$BIN" -c 'koshkit split -a 65' 2>&1

echo "=== split byte count location ==="
"$BIN" -c 'koshkit split -b 0' 2>&1

echo "=== split line count location ==="
"$BIN" -c 'koshkit split -l nope' 2>&1

echo "=== strings minimum location ==="
"$BIN" -c 'koshkit strings -n 0' 2>&1

echo "=== strings radix location ==="
"$BIN" -c 'koshkit strings -t decimal' 2>&1

echo "=== tput row location ==="
"$BIN" -c 'koshkit tput cup nope 1' 2>&1

echo "=== tput column limit ==="
"$BIN" -c 'koshkit tput cup 1 18446744073709551615' 2>&1

echo "=== unexpand tab list location ==="
"$BIN" -c 'koshkit unexpand -t 8,4' 2>&1

echo "=== date operand location ==="
"$BIN" -c 'koshkit date 2026' 2>&1

echo "=== getconf variable location ==="
"$BIN" -c 'koshkit getconf KOSH_MISSING_CONFIGURATION' 2>&1

echo "=== locale name location ==="
"$BIN" -c 'koshkit locale KOSH_MISSING_LOCALE' 2>&1

echo "=== id user location ==="
"$BIN" -c 'koshkit id KOSH_MISSING_USER' 2>&1

echo "=== stty setting location ==="
"$BIN" -c 'koshkit stty KOSH_MISSING_SETTING' 2>&1

echo "=== stty conflicting flag location ==="
"$BIN" -c 'koshkit stty -a -g' 2>&1

echo "=== logger priority location ==="
"$BIN" -c 'koshkit logger -p KOSH_MISSING_PRIORITY message' 2>&1

echo "=== nohup command location ==="
"$BIN" -c 'koshkit nohup KOSH_MISSING_COMMAND' 2>&1

echo "=== evil operand location ==="
"$BIN" -c 'koshkit evil extra' 2>&1

echo "=== evil color value location ==="
"$BIN" -c 'koshkit --color invalid evil' 2>&1

echo "=== evilfs operand location ==="
"$BIN" -c 'koshkit evilfs extra' 2>&1

echo "=== evillogs operand location ==="
"$BIN" -c 'koshkit evillogs extra' 2>&1

echo "=== evilnet operand location ==="
"$BIN" -c 'koshkit evilnet extra' 2>&1

echo "=== evilss operand location ==="
"$BIN" -c 'koshkit evilss extra' 2>&1

echo "=== evilps limit location ==="
"$BIN" -c 'koshkit evilps -0' 2>&1

echo "=== evilps extra operand location ==="
"$BIN" -c 'koshkit evilps 1 2' 2>&1

echo "=== evilps sort value location ==="
"$BIN" -c 'koshkit evilps --sort invalid' 2>&1

echo "=== evilps process location ==="
"$BIN" -c 'koshkit evilps invalid' 2>&1

echo "=== evilps absent process location ==="
"$BIN" -c 'koshkit evilps 9223372036854775807' 2>&1

echo "=== evilio extra operand location ==="
"$BIN" -c 'koshkit evilio 1 2' 2>&1

echo "=== evilio duration mode location ==="
"$BIN" -c 'koshkit evilio 1' 2>&1

echo "=== evilio duration value location ==="
"$BIN" -c 'koshkit evilio --cumulative=0' 2>&1

echo "=== evilio process limit location ==="
"$BIN" -c 'koshkit evilio -0' 2>&1

echo "=== evilio later count location ==="
"$BIN" -c 'koshkit evilio -5 --count 2' 2>&1

echo "=== evilio later compact limit location ==="
"$BIN" -c 'koshkit evilio --count 2 -5' 2>&1

echo "=== evilio count value location ==="
"$BIN" -c 'koshkit evilio --count 0' 2>&1

echo "=== evilio process value location ==="
"$BIN" -c 'koshkit evilio --pid 0' 2>&1

echo "=== evilio color value location ==="
"$BIN" -c 'koshkit --color invalid evilio' 2>&1

echo "=== evilio report mode location ==="
"$BIN" -c 'koshkit evilio --all --cumulative' 2>&1

echo "=== evilio later all location ==="
"$BIN" -c 'koshkit evilio --cumulative --all' 2>&1

echo "=== evilio live mode location ==="
"$BIN" -c 'koshkit evilio --all --live' 2>&1

echo "=== evilio invalid sort location ==="
"$BIN" -c 'koshkit evilio --sort nowhere' 2>&1

echo "=== evilio ambiguous sort location ==="
"$BIN" -c 'koshkit evilio --sort r' 2>&1

echo "=== evilio process sort mode location ==="
"$BIN" -c 'koshkit evilio --ps --sort busy' 2>&1

echo "=== evilio disk sort mode location ==="
"$BIN" -c 'koshkit evilio --sort pid' 2>&1

echo "=== evilio sampled sort mode location ==="
"$BIN" -c 'koshkit evilio --sort busy' 2>&1

echo "=== basename extra operand location ==="
"$BIN" -c 'koshkit basename path suffix extra' 2>&1

echo "=== bc register ranges ==="
"$BIN" -c "printf 'ibase=1\nobase=17\nscale=-1\n' | koshkit bc" 2>&1

echo "=== cal extra operand location ==="
"$BIN" -c 'koshkit cal 1 2024 extra' 2>&1

echo "=== cal month location ==="
"$BIN" -c 'koshkit cal 13 2024' 2>&1

echo "=== cal year location ==="
"$BIN" -c 'koshkit cal 1 0' 2>&1

echo "=== chgrp group location ==="
"$BIN" -c 'koshkit chgrp KOSH_MISSING_GROUP LICENSE' 2>&1

echo "=== chmod mode location ==="
"$BIN" -c 'koshkit chmod u=rz LICENSE' 2>&1

echo "=== chown owner location ==="
"$BIN" -c 'koshkit chown KOSH_MISSING_USER LICENSE' 2>&1

echo "=== chown specification location ==="
"$BIN" -c 'koshkit chown 0: LICENSE' 2>&1

echo "=== chown group location ==="
"$BIN" -c 'koshkit chown :KOSH_MISSING_GROUP LICENSE' 2>&1

echo "=== cut position list location ==="
"$BIN" -c 'koshkit cut -b 0' 2>&1

echo "=== cut delimiter location ==="
"$BIN" -c 'koshkit cut -f 1 -d ::' 2>&1

echo "=== expand tab list location ==="
"$BIN" -c 'koshkit expand -t 8,4' 2>&1

echo "=== fold width location ==="
"$BIN" -c 'koshkit fold -w 0' 2>&1

echo "=== nproc ignore count location ==="
"$BIN" -c 'koshkit nproc --ignore=-1' 2>&1

echo "=== nproc operand location ==="
"$BIN" -c 'koshkit nproc extra' 2>&1

echo "=== evildisk color value location ==="
"$BIN" -c 'koshkit --color invalid evildisk' 2>&1

echo "=== evildisk path location ==="
"$BIN" -c 'koshkit evildisk KOSH_MISSING_FILESYSTEM' 2>&1 >/dev/null

echo "=== evilfiles process value location ==="
"$BIN" -c 'koshkit evilfiles --pid invalid' 2>&1

echo "=== evilfiles user value location ==="
"$BIN" -c 'koshkit evilfiles --user KOSH_MISSING_USER' 2>&1

echo "=== goodfsw path location ==="
"$BIN" -c 'koshkit goodfsw KOSH_MISSING_PATH' 2>&1

echo "=== goodstat color value location ==="
"$BIN" -c 'koshkit --color invalid goodstat LICENSE' 2>&1

echo "=== goodstat path location ==="
"$BIN" -c 'koshkit goodstat KOSH_MISSING_PATH' 2>&1

echo "=== sync later filesystem flag location ==="
"$BIN" -c 'koshkit sync -d -f LICENSE' 2>&1

echo "=== sync later data flag location ==="
"$BIN" -c 'koshkit sync -f -d LICENSE' 2>&1

echo "=== sync path location ==="
"$BIN" -c 'koshkit sync KOSH_MISSING_PATH' 2>&1

echo "=== unlink extra operand location ==="
"$BIN" -c 'koshkit unlink first second' 2>&1

echo "=== unlink missing path location ==="
"$BIN" -c 'koshkit unlink KOSH_MISSING_PATH' 2>&1

echo "=== unlink directory location ==="
"$BIN" -c 'koshkit unlink .' 2>&1

echo "=== tty operand location ==="
"$BIN" -c 'koshkit tty extra' 2>&1

echo "=== uname operand location ==="
"$BIN" -c 'koshkit uname extra' 2>&1

echo "=== who first operand location ==="
"$BIN" -c 'koshkit who wrong' 2>&1

echo "=== who missing second operand location ==="
"$BIN" -c 'koshkit who am' 2>&1

echo "=== who second operand location ==="
"$BIN" -c 'koshkit who am wrong' 2>&1

echo "=== who extra operand location ==="
"$BIN" -c 'koshkit who am i extra' 2>&1

echo "=== killall missing process location ==="
"$BIN" -c 'koshkit killall KOSH_MISSING_PROCESS' 2>&1

echo "=== pkill extra pattern location ==="
"$BIN" -c 'koshkit pkill first second' 2>&1

echo "=== pkill empty pattern location ==="
"$BIN" -c 'koshkit pkill ""' 2>&1

echo "=== link extra operand location ==="
"$BIN" -c 'koshkit link first second third' 2>&1

echo "=== link destination location ==="
"$BIN" -c 'koshkit link KOSH_MISSING_SOURCE KOSH_MISSING_DESTINATION' 2>&1

echo "=== realpath operand location ==="
"$BIN" -c 'koshkit realpath KOSH_MISSING_PATH' 2>&1

echo "=== rmdir operand location ==="
"$BIN" -c 'koshkit rmdir KOSH_MISSING_DIRECTORY' 2>&1

echo "=== sleep duration location ==="
"$BIN" -c 'koshkit sleep invalid' 2>&1

echo "=== watch interval location ==="
"$BIN" -c 'koshkit watch --interval invalid true' 2>&1

echo "=== goodfsw latency location ==="
"$BIN" -c 'koshkit goodfsw --latency invalid .' 2>&1

echo "=== retry delay location ==="
"$BIN" -c 'koshkit retry --delay invalid true' 2>&1

echo "=== timeout duration location ==="
"$BIN" -c 'koshkit timeout invalid true' 2>&1

echo "=== timeout signal location ==="
"$BIN" -c 'koshkit timeout --signal KOSH_MISSING_SIGNAL 1 true' 2>&1

echo "=== timeout signal range location ==="
"$BIN" -c 'koshkit timeout --signal 999999999999999999999 1 true' 2>&1

echo "=== timeout command location ==="
"$BIN" -c 'koshkit timeout 1 KOSH_MISSING_COMMAND' 2>&1

echo "=== killall signal location ==="
"$BIN" -c 'koshkit killall --signal KOSH_MISSING_SIGNAL process' 2>&1

echo "=== pkill signal location ==="
"$BIN" -c 'koshkit pkill --signal KOSH_MISSING_SIGNAL process' 2>&1

echo "=== pkill signal range location ==="
"$BIN" -c 'koshkit pkill --signal 999999999999999999999 process' 2>&1

echo "=== sed implicit script location ==="
"$BIN" -c "koshkit sed 's/a'" 2>&1

echo "=== sed expression location ==="
"$BIN" -c "koshkit sed -e p -e Z" 2>&1

echo "=== sed expression boundary location ==="
"$BIN" -c "koshkit sed -e '1' -e p" 2>&1

echo "=== sed regular expression location ==="
"$BIN" -c "koshkit sed -e's/[//'" 2>&1

echo "=== sed line address limit ==="
"$BIN" -c "koshkit sed -e 18446744073709551616p" 2>&1

echo "=== sed zero line address ==="
"$BIN" -c "koshkit sed -e 0p" 2>&1

echo "=== sed second address location ==="
"$BIN" -c "koshkit sed -e 1,p" 2>&1

echo "=== sed translation delimiter location ==="
"$BIN" -c "koshkit sed -e y" 2>&1

echo "=== sed translation length location ==="
"$BIN" -c "koshkit sed -e y/a/bc/" 2>&1

echo "=== sed substitution delimiter location ==="
"$BIN" -c "koshkit sed -e s" 2>&1

echo "=== sed substitution flag location ==="
"$BIN" -c "koshkit sed -e s/a/b/x" 2>&1

echo "=== sed script file location ==="
"$BIN" -c "koshkit sed -f KOSH_MISSING_SCRIPT" 2>&1

echo "=== tabs interval location ==="
"$BIN" -c 'koshkit tabs -0' 2>&1

echo "=== tabs stop location ==="
"$BIN" -c 'koshkit tabs 4,nope' 2>&1

echo "=== tabs descending stop location ==="
"$BIN" -c 'koshkit tabs 4,3' 2>&1

echo "=== tabs relative stop limit ==="
"$BIN" -c 'koshkit tabs 4,+18446744073709551615' 2>&1

echo "=== tabs extra operand location ==="
"$BIN" -c 'koshkit tabs 4,8 extra' 2>&1

echo "=== xargs argument count location ==="
"$BIN" -c 'koshkit xargs -n nope' 2>&1

echo "=== xargs line count location ==="
"$BIN" -c 'koshkit xargs -L 18446744073709551616' 2>&1

echo "=== xargs byte count location ==="
"$BIN" -c 'koshkit xargs -s 0' 2>&1
