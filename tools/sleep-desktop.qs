# Sleep, as on a Mac (docs/SLEEP-PLAN.md), on a whole desktop: the lid,
# the Start menu's Sleep and the power button each put it to sleep behind
# the lock screen, and it wakes locked. QEMU has no lid: tools/fakelid.py's
# table has one, and a SLEEPTEST=1 kernel moves it when a CPU is added.
#   make ARCH=x86_64 SLEEPTEST=1 test-sleep-desktop
# No `sh` while it sleeps: the programs are stopped, ssh too.
expect myos-login: the login screen is up
until myos-login: logged in | ret
wait 30
key ctrl-alt-t
wait 6
sshkey
sh pgrep -l myos-powerd && pgrep -l myos-idle && ls -l /run/myos/power
shot sleep-1-desktop
# The lid shuts: the lock screen, then sleep. It opens: awake, locked. The
# live account has no password, so Enter opens it. myos-sleepcheck, writing
# a line a second, must find the gap (8 s asleep: it counts 5 or more).
sh setsid -f myos-sleepcheck > /dev/null 2>&1 < /dev/null; sleep 3; tail -n 2 ~/sleep-check.txt
lid
next sleep: asleep
shot sleep-2-asleep
wait 8
lid
next sleep: woken by the lid
wait 3
shot sleep-3-locked
key ret
wait 3
sh ! pgrep -l myos-lock && echo UNLOCKED-LID
sh grep -A 2 ASLEEP ~/sleep-check.txt && pkill -f '[m]yos-sleepcheck'
# Now with a password. The Start menu's Sleep; a key wakes it.
sh echo myos:sleep42 | doas chpasswd && echo PASSWORD-SET
xdo mousemove 60 585 click 1
wait 2
shot sleep-4-menu
xdo mousemove 100 537 click 1
next sleep: asleep
key shift
next sleep: woken by a key
wait 3
shot sleep-5-locked
type wrong
key ret
wait 4
shot sleep-6-wrong
type sleep42
key ret
wait 4
sh ! pgrep -l myos-lock && echo UNLOCKED-MENU
shot sleep-7-unlocked
# The power button: asleep; pressed again: awake, locked.
powerbtn
next sleep: asleep
powerbtn
next sleep: woken by the power button
wait 3
shot sleep-8-locked
type sleep42
key ret
wait 4
sh ! pgrep -l myos-lock && echo UNLOCKED-BUTTON
