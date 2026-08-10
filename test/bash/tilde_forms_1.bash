#!/bin/bash
# The accepted alternative for tilde_forms.bash on a bash that expands a bare ~
# to the real home rather than the HOME the script sets, the macOS system bash
# 3.2 among them. kosh follows POSIX and uses the assigned HOME, so the three
# bare-tilde lines are spelled as their literal expansion here, while the rest
# stay dynamic since kosh and that bash agree on them.
HOME=/usr/xyz
echo /usr/xyz
echo /usr/xyz/foo
echo /usr/xyz/foo
echo ~ch\et
echo ~ch\et/foo
echo "~chet"/"foo"
echo \~chet/"foo"
echo ~\chet/bar
echo ~chet""/bar
echo ~nosuchuserhopefully/bar
cd /tmp && cd / && echo "plus=$(cd /tmp && echo ~+)" "minus=~-"
