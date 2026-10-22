# Dimming and the screen off (live/myos-idle) on a whole desktop, with
# tools/fakebat.py's battery (unplugged) and panel (brightness 0-10):
#   QEMU_EXTRA='-acpitable file=build/x86_64/fakebat.aml' live/tools/qemu-desktop.py \
#     build/x86_64/myos-live-uefi.img live/tools/idle-desktop.qs LOG DIR OVMF_CODE OVMF_VARS
# The waits are `sleep` in the machine, not `wait`: X counts idle time on the
# machine's clock, and in a busy QEMU that runs slower than the Mac's
# (docs/gaps.md, the clock counts timer interrupts). Expected: BL-30S 10,
# BL-45S 3, BL-65S 0, BL-BACK 10, BL-BALANCED-50S 10, BL-LOWPOWER-45S 3.
expect myos-login: the login screen is up
until myos-login: logged in | ret
wait 30
key ctrl-alt-t
wait 6
sshkey
sh ls -l /sys/class/backlight/acpi_video0/brightness; pgrep -l myos-idle
# Best battery life on battery: dim at 40 s without input, off at 60 s.
sh echo low-power > /sys/firmware/acpi/platform_profile
xdo mousemove 400 300
sh sleep 30; echo BL-30S $(cat /sys/class/backlight/acpi_video0/brightness)
sh sleep 15; echo BL-45S $(cat /sys/class/backlight/acpi_video0/brightness)
shot idle-dimmed
sh sleep 20; echo BL-65S $(cat /sys/class/backlight/acpi_video0/brightness)
shot idle-off
xdo mousemove 200 200
sh sleep 1; echo BL-BACK $(cat /sys/class/backlight/acpi_video0/brightness)
shot idle-back
# Balanced on battery waits 4 min 40 s: the 40 s alarm only moves on.
sh echo balanced > /sys/firmware/acpi/platform_profile
xdo mousemove 300 300
sh sleep 50; echo BL-BALANCED-50S $(cat /sys/class/backlight/acpi_video0/brightness)
# Back to Best battery life: the next touch starts the 40 s wait again.
sh echo low-power > /sys/firmware/acpi/platform_profile
xdo mousemove 350 300
sh sleep 45; echo BL-LOWPOWER-45S $(cat /sys/class/backlight/acpi_video0/brightness)
