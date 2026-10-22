# The battery in the tray on a whole desktop (docs/BATTERY-PLAN.md), with
# tools/fakebat.py's battery:
#   QEMU_EXTRA='-acpitable file=build/fakebat.aml' live/tools/qemu-desktop.py ...
# Positions are the 800x600 screen at 100%: the tray starts at x 649, the
# battery at 649+61, y 560+11, 18 px (live/theme/make-live-theme.py).
expect myos-login: the login screen is up
until myos-login: logged in | ret      # the live user "myos" (no password on the live image)
wait 40                 # the desktop, the tray, the icon
shot battery-tray
moveto 719 580          # the battery
click
wait 4                  # the pop-up
shot battery-popup
key esc
wait 1
key ctrl-alt-t          # a terminal, for the ssh key
wait 6
sshkey
sh myos-battery --print
sh ls -l /sys/firmware/acpi/platform_profile; id
sh echo performance > /sys/firmware/acpi/platform_profile && myos-battery --print | tail -1
