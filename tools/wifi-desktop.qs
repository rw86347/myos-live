# The Wi-Fi icon on a whole desktop (docs/WIFI-PLAN.md W4), on a kernel with
# the simulated radio (make ARCH=x86_64 WIFISIM=1 test-wifi-desktop): log
# in, open the list from the tray, join the WPA2 network with its key.
# Positions are the 800x600 screen at 100%: the icon sits in the tray's
# network slot (x 649+13, y 560+11, 18 px; live/theme/make-live-theme.py),
# the list against the screen's right edge, 320 x 420.
expect myos-login: the login screen is up
until myos-login: logged in | ret      # the live user "myos" (no password on the live image)
wait 40                 # the desktop, the tray, the icon
moveto 671 580          # the Wi-Fi icon
click
wait 5                  # the list opens, and scans
shot wifi-list
key tab                 # the first network (the strongest: MyOS-Test)
key ret                 # chosen: its Connect button has the keyboard
wait 1
key ret                 # Connect: it asks for the key
wait 1
type myos-wifi-test     # the key (drivers/net/wireless/sim.c, SIM_PASSPHRASE)
shot wifi-key
key ret                 # Next
wait 20                 # the handshake, the kernel's DHCP
shot wifi-connected
