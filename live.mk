# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026 Rodger Wilson
# live.mk — the live and installer disks: their software, the images, their
# tests and VMs. This is the myos-live repo, checked out at live/ inside the
# MyOS (kernel) checkout; MyOS's Makefile includes this file when it is
# there, so every target here is run from the MyOS folder as before:
#     make ARCH=x86_64 live-uefi        make live-hd        make test-install
# Paths are relative to the MyOS folder: kernel files as they are
# (arch/..., tools/mkuefi.py, $(BUILD)/kernel.bin), this repo's under live/.

# Login-only initramfs (rules sit with live-hd: they need LOGIN_BIN).
# x86_64 paths match live/tools/fetch-live.py --arch x86_64 (not build/x86_64/live).
ifeq ($(ARCH),x86_64)
  LIVE_SYSROOT      := build/live/sysroot-x86_64
  LIVE_EXTRA        := build/live/extra.x86_64
  LIVE_TAR          := build/live/rootfs-x86_64.tar
  LIVE_LOGIN_TAR    := build/live/login-root-x86_64.tar
  LIVE_LOGIN_INITRD := build/live/login-initrd-x86_64.cpio
  LIVE_LOCK         := live/packages.x86_64.lock
  LIVE_HD           := $(BUILD)/myos-live-root.img
  LIVE_MB           := 4096
  MUSL_LDSO         := /lib/ld-musl-x86_64.so.1
  LIVE_MARCH        := -march=x86-64
  ALPINE_TRIPLET    := x86_64-alpine-linux-musl
  MKUEFI_ROOT       := --root-file $(LIVE_HD)
else
  LIVE_SYSROOT      := $(BUILD)/live/sysroot
  LIVE_EXTRA        := $(BUILD)/live/extra
  LIVE_TAR          := $(BUILD)/live/rootfs.tar
  LIVE_LOGIN_TAR    := $(BUILD)/live/login-root.tar
  LIVE_LOGIN_INITRD := $(BUILD)/live/login-initrd.cpio
  LIVE_LOCK         := live/packages.lock
  LIVE_HD           := $(BUILD)/myos-live.img
  LIVE_MB           := 1536
  MUSL_LDSO         := /lib/ld-musl-i386.so.1
  LIVE_MARCH        := -march=i686
  ALPINE_TRIPLET    := i586-alpine-linux-musl
  MKUEFI_ROOT       := --root-file $(LIVE_HD) --root-from-mbr
endif

# Live desktop on the same GPT disk: ESP + ext2 "myos-root" from make live-hd.
# The ESP carries the login initrd so a machine with no visible disks still
# reaches myos-login from RAM. QEMU still mounts the ext2 partition.
# x86_64: full kernel + login initrd; userland may still need syscalls (see
# docs/x86_64-LIVE-HANDOFF.md) before myos-login appears.
LIVE_UEFI := $(BUILD)/myos-live-uefi.img
# Then its first boot runs here, once, in QEMU (live/tools/live-prepare.py): the
# caches and databases a fresh disk used to make at every stick's first boot
# (10-15 s in QEMU, minutes on a Pentium 4). LIVE_PREPARE=0 skips it (the
# disk then does it at its own first boot, as before).
LIVE_PREPARE ?= 1
$(LIVE_UEFI): $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(LIVE_LOGIN_INITRD) $(LIVE_HD) \
              arch/i386/boot/efi/vga-font.bin tools/mkuefi.py live/tools/live-prepare.py
	ESP=$$(( $$(wc -c < $(LIVE_LOGIN_INITRD)) / 1048576 + 32 )); \
	  [ $$ESP -lt 128 ] && ESP=128; \
	  python3 tools/mkuefi.py --out $@ --esp-mb $$ESP $(MKUEFI_ROOT) --config-mb 1 \
	    --file EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64.EFI \
	    --file EFI/MYOS/KERNEL.BIN=$(BUILD)/kernel.bin \
	    --file EFI/MYOS/INITRD.CPIO=$(LIVE_LOGIN_INITRD) \
	    --file EFI/MYOS/FONT.BIN=arch/i386/boot/efi/vga-font.bin
	@if [ "$(LIVE_PREPARE)" != 0 ]; then \
	  python3 live/tools/live-prepare.py $@ --debugfs $(DEBUGFS) --e2fsck $(E2FSCK) \
	    --ovmf-code $(OVMF_CODE) --ovmf-vars $(OVMF_VARS); fi
live-uefi: $(LIVE_UEFI)

test-live-uefi: $(LIVE_UEFI)
	@rm -f $(BUILD)/live-uefi-serial.log
	@cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(sleep 50; echo quit) \
	  | $(QEMU64) -machine q35 -m 1024M -cpu qemu64 \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
	    -drive if=none,format=raw,file=$(LIVE_UEFI),id=bootdisk \
	    -device ide-hd,drive=bootdisk,bootindex=0 \
	    -net none -no-reboot -display none \
	    -serial file:$(BUILD)/live-uefi-serial.log -monitor stdio >/dev/null || true
	@cat $(BUILD)/live-uefi-serial.log; echo
	@grep -q "Hello world from MyOS" $(BUILD)/live-uefi-serial.log \
	  || { echo "FAIL: live UEFI did not start the kernel"; exit 1; }
	@grep -q "type 83 Linux" $(BUILD)/live-uefi-serial.log \
	  || { echo "FAIL: GPT Linux partition not seen"; exit 1; }
	@grep -q "mounted at /" $(BUILD)/live-uefi-serial.log \
	  || { echo "FAIL: myos-root did not become /"; exit 1; }
	@echo "LIVE UEFI OK: OVMF started the kernel and mounted myos-root"

# The live disk as a USB stick, the only disk (the laptop case): OVMF starts
# MyOS from it, our xHCI driver finds the stick again, its myos-root becomes
# "/", and the login screen comes up from it. docs/USB-PLAN.md.
test-usb: $(LIVE_UEFI)
	@rm -f $(BUILD)/usb-serial.log
	@cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(for i in $$(seq 150); do sleep 1; \
	    grep -q "login screen is up" $(BUILD)/usb-serial.log 2>/dev/null && break; done; \
	  echo quit) \
	  | $(QEMU64) -machine q35,pit=off -m 4G -cpu qemu64 -vga cirrus \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
	    -device qemu-xhci,id=xhci \
	    -drive if=none,format=raw,file=$(LIVE_UEFI),id=stick,snapshot=on \
	    -device usb-storage,bus=xhci.0,drive=stick,bootindex=0 \
	    -net none -no-reboot -display none \
	    -serial file:$(BUILD)/usb-serial.log -monitor stdio >/dev/null || true
	@L=$(BUILD)/usb-serial.log; ok=1; \
	  grep -a "xhci\|usb\|sd[a-z]\|mounted at /\|login screen" $$L; \
	  grep -q "^ *xhci: .*MSI-X" $$L || { echo "FAIL: xHCI with MSI-X"; ok=0; }; \
	  grep -q "^ *usb-storage: sda: " $$L || { echo "FAIL: the stick as sda"; ok=0; }; \
	  grep -q "^ *ext2: sda2 mounted at /:" $$L || { echo "FAIL: myos-root from the stick"; ok=0; }; \
	  grep -q "the login screen is up" $$L || { echo "FAIL: the login screen"; ok=0; }; \
	  [ $$ok = 1 ] && echo "USB TEST OK ($(ARCH)): the live system runs from a USB stick" || exit 1

# The installer, end to end (live/tools/test-install.py; docs/INSTALLER-PLAN.md):
# the live stick installs onto an empty disk in QEMU, through the installer
# window's own helper, over the serial console; e2fsck checks the result;
# the disk then starts alone and the new account logs in. One run per
# MACHINE:DISK. x86_64: UEFI machines (q35; p2max = no PS/2, USB input) onto
# nvme, sata or usb. i386: an old BIOS PC (pc) onto ide or sata, an MBR
# disk. Each run writes ~4 GB to a sparse disk in $(BUILD)/test-install and
# deletes it after.
ifeq ($(ARCH),x86_64)
INSTALL_TESTS ?= q35:nvme q35:sata q35:usb p2max:nvme
INSTALL_IMG   := $(LIVE_UEFI)
INSTALL_FW    := --ovmf-code $(OVMF_CODE) --ovmf-vars $(OVMF_VARS)
else
INSTALL_TESTS ?= pc:ide pc:sata
INSTALL_IMG   := $(LIVE_HD)
INSTALL_FW    :=
endif
test-install: $(INSTALL_IMG)
	@ok=1; for t in $(INSTALL_TESTS); do \
	  python3 live/tools/test-install.py --image $(INSTALL_IMG) $(INSTALL_FW) \
	    --e2fsck $(E2FS)/e2fsck --work $(BUILD)/test-install \
	    --machine $${t%%:*} --disk $${t##*:} || ok=0; done; \
	  [ $$ok = 1 ] && echo "INSTALL TEST OK ($(ARCH)): $(INSTALL_TESTS)" || exit 1

# Upgrade from the stick onto a disk this installer already made
# (live/tools/test-install.py --upgrade). One UEFI and one BIOS run by default.
ifeq ($(ARCH),x86_64)
UPGRADE_TESTS ?= q35:nvme
else
UPGRADE_TESTS ?= pc:ide
endif
test-upgrade: $(INSTALL_IMG)
	@ok=1; for t in $(UPGRADE_TESTS); do \
	  python3 live/tools/test-install.py --upgrade --image $(INSTALL_IMG) $(INSTALL_FW) \
	    --e2fsck $(E2FS)/e2fsck --debugfs $(DEBUGFS) --work $(BUILD)/test-upgrade \
	    --machine $${t%%:*} --disk $${t##*:} || ok=0; done; \
	  [ $$ok = 1 ] && echo "UPGRADE TEST OK ($(ARCH)): $(UPGRADE_TESTS)" || exit 1

# RAM-only login (no ext2): what a real UEFI laptop does until USB MSC exists.
UEFI_LOGIN_IMG := $(BUILD)/myos-uefi-login.img
$(UEFI_LOGIN_IMG): $(BUILD)/BOOTX64.EFI $(BUILD)/kernel.bin $(LIVE_LOGIN_INITRD) \
                   arch/i386/boot/efi/vga-font.bin tools/mkuefi.py
	ESP=$$(( $$(wc -c < $(LIVE_LOGIN_INITRD)) / 1048576 + 32 )); \
	  [ $$ESP -lt 128 ] && ESP=128; \
	  python3 tools/mkuefi.py --out $@ --esp-mb $$ESP \
	    --file EFI/BOOT/BOOTX64.EFI=$(BUILD)/BOOTX64.EFI \
	    --file EFI/MYOS/KERNEL.BIN=$(BUILD)/kernel.bin \
	    --file EFI/MYOS/INITRD.CPIO=$(LIVE_LOGIN_INITRD) \
	    --file EFI/MYOS/FONT.BIN=arch/i386/boot/efi/vga-font.bin
# Laptop-like (the MSI Katana, docs/hardware-msi-katana-gf66.md): 4 GiB, a
# Cirrus card we have no driver for, so the screen is the firmware's
# framebuffer (simplefb), and no PIT (pit=off: the HPET must give the tick).
# Then the login screen must really be showing (live/tools/check-xscreen.py).
test-uefi-login: $(UEFI_LOGIN_IMG)
	@rm -f $(BUILD)/uefi-login-serial.log $(BUILD)/uefi-login-screen.ppm
	@cp $(OVMF_VARS) $(BUILD)/ovmf-vars.fd
	@(sleep 110; echo "screendump $(BUILD)/uefi-login-screen.ppm"; sleep 2; echo quit) \
	  | $(QEMU64) -machine q35,pit=off -m 4G -cpu qemu64 -vga cirrus \
	    -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
	    -drive if=pflash,format=raw,file=$(BUILD)/ovmf-vars.fd \
	    -drive if=none,format=raw,file=$(UEFI_LOGIN_IMG),id=bootdisk \
	    -device ide-hd,drive=bootdisk,bootindex=0 \
	    -net none -no-reboot -display none \
	    -serial file:$(BUILD)/uefi-login-serial.log -monitor stdio >/dev/null || true
	@cat $(BUILD)/uefi-login-serial.log; echo
	@grep -q "Hello world from MyOS" $(BUILD)/uefi-login-serial.log \
	  || { echo "FAIL: login UEFI did not start the kernel"; exit 1; }
	@grep -q "initramfs: [1-9][0-9][0-9][0-9] files" $(BUILD)/uefi-login-serial.log \
	  || { echo "FAIL: login initrd did not unpack"; exit 1; }
	@grep -qE "MyOS is up|First boot:|/init exited|myos-login" $(BUILD)/uefi-login-serial.log \
	  || { echo "FAIL: runit/myos-login did not start"; exit 1; }
	@python3 live/tools/check-xscreen.py $(BUILD)/uefi-login-screen.ppm \
	  || { echo "FAIL: the X login screen is not showing"; exit 1; }
	@grep -q "the HPET gives the 1 ms tick" $(BUILD)/uefi-login-serial.log \
	  || { echo "FAIL: no HPET tick without the PIT"; exit 1; }
	@echo "UEFI LOGIN OK: the X login screen shows (simplefb, 4 GiB, no PIT)"

# The Wi-Fi icon on a whole desktop (docs/WIFI-PLAN.md W4): boots the RAM
# login image on a kernel with the simulated radio, logs in, and joins the
# WPA2 network through the tray icon's list, as a person would
# (live/tools/wifi-desktop.qs); the screenshots land in $(BUILD).
test-wifi-desktop: $(UEFI_LOGIN_IMG)
	@[ "$(WIFISIM)" = 1 ] && [ "$(ARCH)" = x86_64 ] \
	  || { echo "test-wifi-desktop: make ARCH=x86_64 WIFISIM=1 test-wifi-desktop"; exit 1; }
	python3 live/tools/qemu-desktop.py $(UEFI_LOGIN_IMG) live/tools/wifi-desktop.qs \
	  $(BUILD)/wifi-desktop-serial.log $(BUILD) $(OVMF_CODE) $(OVMF_VARS)
	@grep -q "wifi: the simulated radio is up" $(BUILD)/wifi-desktop-serial.log \
	  || { echo "FAIL: no simulated radio"; exit 1; }
	@grep -q "ipv4: 192.168.77.10 on wlan0" $(BUILD)/wifi-desktop-serial.log \
	  || { echo "FAIL: not connected through the tray icon (see $(BUILD)/wifi-*.png)"; exit 1; }
	@echo "WIFI DESKTOP OK: joined MyOS-Test from the tray icon (screenshots: $(BUILD)/wifi-*.png)"

# Sleep on a whole desktop (docs/SLEEP-PLAN.md): the live disk on a kernel
# whose lid (tools/fakelid.py's) moves when a CPU is added. The lid, the
# Start menu's Sleep and the power button each put it to sleep behind the
# lock screen, and it is unlocked after each wake, without and then with a
# password (live/tools/sleep-desktop.qs); the screenshots land in $(BUILD).
test-sleep-desktop: $(LIVE_UEFI)
	@[ "$(SLEEPTEST)" = 1 ] && [ "$(ARCH)" = x86_64 ] \
	  || { echo "test-sleep-desktop: make ARCH=x86_64 SLEEPTEST=1 test-sleep-desktop"; exit 1; }
	python3 tools/fakelid.py $(BUILD)/fakelid.aml
	QEMU_EXTRA='-acpitable file=$(BUILD)/fakelid.aml -smp 1,maxcpus=4' \
	  python3 live/tools/qemu-desktop.py $(LIVE_UEFI) live/tools/sleep-desktop.qs \
	  $(BUILD)/sleep-desktop-serial.log $(BUILD) $(OVMF_CODE) $(OVMF_VARS)
	@L=$(BUILD)/sleep-desktop-serial.log; ok=1; \
	  for w in "the lid" "a key" "the power button"; do \
	    grep -q "sleep: woken by $$w" $$L || { echo "FAIL: not woken by $$w"; ok=0; }; done; \
	  [ $$ok = 1 ] && echo "SLEEP DESKTOP OK: the lid, the Start menu and the power button" \
	    "(screenshots: $(BUILD)/sleep-*.png)" || exit 1

# --- the live disk's software (step 9c-3) ----------------------------------------
# Every program on the live disk (live/packages.txt: IceWM desktop, logins,
# AbiWord, gcc/make/python3/adb...), as pinned Alpine packages
# (live/packages.lock), plus the MyOS source, in one archive:
# build/live/rootfs.tar. `live/tools/fetch-live.py --update` re-resolves the list.
live-root: live-extra
	python3 live/tools/fetch-live.py --arch $(ALPINE_ARCH)

# What MyOS itself puts on the live disk, on top of the packages
# (build/live/extra, copied in by live/tools/fetch-live.py):
#   the Golden Hour theme    IceWM's theme and the login screen's pictures,
#                            drawn from the palette in live/assets/themes
#   myos-login               the graphical login screen (live/myos-login)
# myos-login is a Linux program, not kernel code: it is compiled against
# Alpine's own headers and libraries, unpacked into build/live/sysroot by
# `live/tools/fetch-live.py --sysroot`, and linked the way every program on the
# disk is -- dynamically, against musl's loader. (PAM needs that: it opens
# its modules at run time.)
LOGIN_BIN    := $(LIVE_EXTRA)/usr/bin/myos-login
GCC_INC      := $(shell $(CC) -print-file-name=include)
LOGIN_CFLAGS := -std=c11 -O2 -Wall -Wextra $(LIVE_MARCH) -D_GNU_SOURCE -nostdinc \
                -isystem $(LIVE_SYSROOT)/usr/include \
                -isystem $(LIVE_SYSROOT)/usr/include/freetype2 -isystem $(GCC_INC)
LOGIN_LDLIBS := -lX11 -lXft -lImlib2 -lpam -lc -lgcc

$(LIVE_SYSROOT)/.done:
	python3 live/tools/fetch-live.py --arch $(ALPINE_ARCH) --sysroot

$(LOGIN_BIN): live/myos-login/myos-login.c live/common/xpic.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Ilive/common -c $< -o $(BUILD)/live/myos-login.o
	$(CC) -nostdlib -Wl,-z,noexecstack -Wl,-dynamic-linker,$(MUSL_LDSO) \
	  -Wl,-rpath-link,$(LIVE_SYSROOT)/usr/lib:$(LIVE_SYSROOT)/lib \
	  $(LIVE_SYSROOT)/usr/lib/crt1.o $(LIVE_SYSROOT)/usr/lib/crti.o \
	  $(BUILD)/live/myos-login.o -L$(LIVE_SYSROOT)/usr/lib -L$(LIVE_SYSROOT)/lib \
	  $(LOGIN_LDLIBS) $(LIVE_SYSROOT)/usr/lib/crtn.o -o $@

# Mission Control for the X11 desktop (live/myos-mission): myos-menu starts
# it from the page curl. The layout is the Wayland session's (live/myos-wm/layout.c).
MISSION_BIN := $(LIVE_EXTRA)/usr/bin/myos-mission
$(MISSION_BIN): live/myos-mission/myos-mission.c live/myos-wm/layout.c live/myos-wm/layout.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Ilive/myos-wm -c live/myos-mission/myos-mission.c -o $(BUILD)/live/myos-mission.o
	$(CC) $(LOGIN_CFLAGS) -Ilive/myos-wm -c live/myos-wm/layout.c -o $(BUILD)/live/myos-mission-layout.o
	$(call LIVE_LINK,$(BUILD)/live/myos-mission.o $(BUILD)/live/myos-mission-layout.o,\
	  -lX11 -lXrender -lXft -l:libXcomposite.so.1)

# The Start menu (live/myos-menu): the same libraries, less PAM.
MENU_BIN := $(LIVE_EXTRA)/usr/bin/myos-menu
$(MENU_BIN): live/myos-menu/myos-menu.c live/myos-menu/programs.c live/myos-menu/programs.h \
             live/common/xpic.h live/myos-battery/power_supply.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Ilive/common -Ilive/myos-menu -Ilive/myos-battery -c live/myos-menu/myos-menu.c -o $(BUILD)/live/myos-menu.o
	$(CC) $(LOGIN_CFLAGS) -Ilive/common -Ilive/myos-menu -c live/myos-menu/programs.c -o $(BUILD)/live/programs.o
	$(CC) -nostdlib -Wl,-z,noexecstack -Wl,-dynamic-linker,$(MUSL_LDSO) \
	  -Wl,-rpath-link,$(LIVE_SYSROOT)/usr/lib:$(LIVE_SYSROOT)/lib \
	  $(LIVE_SYSROOT)/usr/lib/crt1.o $(LIVE_SYSROOT)/usr/lib/crti.o \
	  $(BUILD)/live/myos-menu.o $(BUILD)/live/programs.o -L$(LIVE_SYSROOT)/usr/lib -L$(LIVE_SYSROOT)/lib \
	  -lX11 -lXft -lImlib2 -l:libXfixes.so.3 -l:libXi.so.6 -lc -lgcc $(LIVE_SYSROOT)/usr/lib/crtn.o -o $@

# Settings (live/myos-settings): a GTK3 program, and myos-xsettings, which
# applies the desktop's scale. GTK's headers come from packages.txt's
# [build-only] group; its libraries are on the disk already.
SETTINGS_BIN  := $(LIVE_EXTRA)/usr/bin/myos-settings
XSETTINGS_BIN := $(LIVE_EXTRA)/usr/bin/myos-xsettings
GTK_CFLAGS := $(addprefix -isystem $(LIVE_SYSROOT)/usr/include/,gtk-3.0 glib-2.0 pango-1.0 cairo \
                gdk-pixbuf-2.0 atk-1.0 harfbuzz webkitgtk-4.1 javascriptcoregtk-4.1 libsoup-3.0) \
                -isystem $(LIVE_SYSROOT)/usr/lib/glib-2.0/include
LIVE_LINK = $(CC) -nostdlib -Wl,-z,noexecstack -Wl,-dynamic-linker,$(MUSL_LDSO) \
	  -Wl,-rpath-link,$(LIVE_SYSROOT)/usr/lib:$(LIVE_SYSROOT)/lib \
	  $(LIVE_SYSROOT)/usr/lib/crt1.o $(LIVE_SYSROOT)/usr/lib/crti.o \
	  $(1) -L$(LIVE_SYSROOT)/usr/lib -L$(LIVE_SYSROOT)/lib \
	  $(2) -lc -lgcc $(LIVE_SYSROOT)/usr/lib/crtn.o -o $@
# Settings › Kernel (docs/KERNEL-CONFIG.md): kconfig.c is shared with the
# helper that saves it; its table of power savings is made from
# tools/kconfig/powerfeatures.py, so the two can't drift apart.
KCONFIG_FEATURES := $(BUILD)/live/kconfig_features.h
$(KCONFIG_FEATURES): tools/kconfig/powerfeatures.py
	@mkdir -p $(dir $@)
	python3 $< --c > $@
$(BUILD)/live/kconfig.o: live/myos-settings/kconfig.c live/myos-settings/kconfig.h $(KCONFIG_FEATURES) \
                         | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(CC) $(LOGIN_CFLAGS) -I$(BUILD)/live -Wno-format-truncation -c $< -o $@
$(SETTINGS_BIN): live/myos-settings/myos-settings.c live/myos-settings/scale.h \
                 live/myos-settings/wallpaper.h live/myos-settings/kconfig.h $(BUILD)/live/kconfig.o \
                 | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Wno-deprecated-declarations -Wno-format-truncation -c $< -o $(BUILD)/live/myos-settings.o
	$(call LIVE_LINK,$(BUILD)/live/myos-settings.o $(BUILD)/live/kconfig.o,-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lgio-2.0 \
	  -lgobject-2.0 -lglib-2.0 -lcairo -lpango-1.0 -lpangocairo-1.0 -lX11 -lm)
# The Wi-Fi tray icon (live/myos-wifi): GTK3, wpa_supplicant's control
# socket. Started by IceWM's startup script.
WIFI_BIN := $(LIVE_EXTRA)/usr/bin/myos-wifi
$(WIFI_BIN): live/myos-wifi/myos-wifi.c live/myos-settings/scale.h live/common/menu_bubble.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Ilive/myos-settings -Ilive/common -Wno-deprecated-declarations \
	  -c $< -o $(BUILD)/live/myos-wifi.o
	$(call LIVE_LINK,$(BUILD)/live/myos-wifi.o,-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lgobject-2.0 \
	  -lglib-2.0 -lcairo -lpango-1.0 -lX11 -lm)
# The desktop icons (live/myos-desktop): files in ~/Desktop, and the
# right-click that opens Settings > Wallpaper. Started by IceWM's startup.
DESKTOP_BIN := $(LIVE_EXTRA)/usr/bin/myos-desktop
$(DESKTOP_BIN): live/myos-desktop/myos-desktop.c live/myos-settings/scale.h \
                live/myos-settings/wallpaper.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -isystem $(LIVE_SYSROOT)/usr/include/librsvg-2.0 \
	  -isystem $(LIVE_SYSROOT)/usr/include/poppler/glib \
	  -Ilive/myos-settings -Wno-deprecated-declarations -Wno-format-truncation \
	  -c live/myos-desktop/myos-desktop.c -o $(BUILD)/live/myos-desktop.o
	$(call LIVE_LINK,$(BUILD)/live/myos-desktop.o,-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lgio-2.0 \
	  -lgobject-2.0 -lglib-2.0 -lpango-1.0 -lpangocairo-1.0 -lcairo -lrsvg-2 -lpoppler-glib -lX11 -lm)
# The Wayland window server (live/myos-wm): wlroots, cairo and pango for
# the title bars and Mission Control, gdk-pixbuf for icons and the
# wallpaper. Its xdg-shell and layer-shell headers are generated once
# (wayland-scanner) and kept in live/myos-wm. Mission Control's layout is
# checked on the host: `make wm-layout-test`.
WM_BIN  := $(LIVE_EXTRA)/usr/bin/myos-wm
WM_SRCS := main view input draw mission layout
WM_OBJS := $(WM_SRCS:%=$(BUILD)/live/myos-wm/%.o)
WM_CFLAGS := $(LOGIN_CFLAGS) -DWLR_USE_UNSTABLE -Ilive/myos-wm \
             $(addprefix -isystem $(LIVE_SYSROOT)/usr/include/,pixman-1 libdrm cairo pango-1.0 \
                harfbuzz glib-2.0 gdk-pixbuf-2.0) -isystem $(LIVE_SYSROOT)/usr/lib/glib-2.0/include
$(WM_OBJS): $(BUILD)/live/myos-wm/%.o: live/myos-wm/%.c live/myos-wm/wm.h live/myos-wm/layout.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(CC) $(WM_CFLAGS) -c $< -o $@
$(WM_BIN): $(WM_OBJS)
	@mkdir -p $(dir $@)
	$(call LIVE_LINK,$(WM_OBJS),-lwlroots -lwayland-server -lxkbcommon -lpixman-1 -lcairo \
	  -lpango-1.0 -lpangocairo-1.0 -lgdk_pixbuf-2.0 -lgobject-2.0 -lglib-2.0 -lm)
wm-layout-test: live/myos-wm/layout.c live/myos-wm/layout_test.c live/myos-wm/layout.h
	@mkdir -p $(BUILD)/live
	cc -std=c11 -O2 -Wall -Wextra live/myos-wm/layout.c live/myos-wm/layout_test.c -o $(BUILD)/live/layout_test
	$(BUILD)/live/layout_test
.PHONY: wm-layout-test
# My Files (live/myos-files): GTK3 for the window, cairo and pango for
# the drawing, librsvg for the theme's SVG icons.
FILES_BIN := $(LIVE_EXTRA)/usr/bin/myos-files
HEIF_PIXBUF_OBJ := $(BUILD)/live/heif-pixbuf.o
$(HEIF_PIXBUF_OBJ): live/myos-heic/heif-pixbuf.c live/myos-heic/heif-pixbuf.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Ilive/myos-heic -Wno-deprecated-declarations \
	  -c $< -o $@
$(FILES_BIN): live/myos-files/myos-files.c live/myos-settings/scale.h $(HEIF_PIXBUF_OBJ) | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -isystem $(LIVE_SYSROOT)/usr/include/librsvg-2.0 \
	  -isystem $(LIVE_SYSROOT)/usr/include/poppler/glib \
	  -Ilive/myos-settings -Ilive/myos-heic -Wno-deprecated-declarations -Wno-format-truncation \
	  -c live/myos-files/myos-files.c -o $(BUILD)/live/myos-files.o
	$(call LIVE_LINK,$(BUILD)/live/myos-files.o $(HEIF_PIXBUF_OBJ),-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 \
	  -lpoppler-glib -lheif -lgobject-2.0 -lglib-2.0 -lgio-2.0 -lpango-1.0 -lpangocairo-1.0 \
	  -lcairo -lrsvg-2 -lX11 -lm)
# rm (live/myos-rm): people's rm moves to ~/Trash; root and system accounts
# get GNU rm (coreutils). /bin/rm and /usr/bin/rm are symlinks to it, laid
# over Alpine's symlink to coreutils: a symlink replaces a symlink when the
# tar is unpacked, where a file would be written through it (into coreutils).
RM_BIN := $(LIVE_EXTRA)/usr/bin/myos-rm
$(RM_BIN): live/myos-rm/myos-rm.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(LIVE_EXTRA)/bin $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Wno-format-truncation -c $< -o $(BUILD)/live/myos-rm.o
	$(call LIVE_LINK,$(BUILD)/live/myos-rm.o,)
	ln -sfn /usr/bin/myos-rm $(LIVE_EXTRA)/bin/rm
	ln -sfn /usr/bin/myos-rm $(LIVE_EXTRA)/usr/bin/rm
# The Recycle can on the desktop: drops go to rm, Empty is --emptyTrash.
TRASH_BIN := $(LIVE_EXTRA)/usr/bin/myos-trash
$(TRASH_BIN): live/myos-trash/myos-trash.c live/myos-settings/scale.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -isystem $(LIVE_SYSROOT)/usr/include/librsvg-2.0 \
	  -Ilive/myos-settings -Wno-deprecated-declarations \
	  -c $< -o $(BUILD)/live/myos-trash.o
	$(call LIVE_LINK,$(BUILD)/live/myos-trash.o,-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lgobject-2.0 \
	  -lglib-2.0 -lgio-2.0 -lcairo -lrsvg-2 -lX11 -lm)
# The speaker in the tray (live/myos-volume): the volume pop-up, and the
# wheel's steps. The sound card's controls through include/sound/asound.h
# (quoted: Alpine's linux-headers has its own, with other field names).
VOLUME_BIN := $(LIVE_EXTRA)/usr/bin/myos-volume
$(VOLUME_BIN): live/myos-volume/myos-volume.c include/sound/asound.h live/common/menu_bubble.h \
             live/common/tray_follow.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -iquote include/sound -Ilive/common -Wno-deprecated-declarations \
	  -c $< -o $(BUILD)/live/myos-volume.o
	$(call LIVE_LINK,$(BUILD)/live/myos-volume.o,-lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lcairo)
# The battery in the tray (live/myos-battery): the pop-up with the charge,
# the time left, health and the power-saving slider (platform_profile).
BATTERY_BIN := $(LIVE_EXTRA)/usr/bin/myos-battery
$(BATTERY_BIN): live/myos-battery/myos-battery.c live/myos-battery/power_supply.h \
             live/common/tray_follow.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Ilive/common -Wno-deprecated-declarations \
	  -c $< -o $(BUILD)/live/myos-battery.o
	$(call LIVE_LINK,$(BUILD)/live/myos-battery.o,-lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lcairo)
# Ctrl+Space between the keyboards, and its bubble (live/myos-input). IBus
# is on the x86_64 disk only; libibus is linked by its file name (the
# sysroot has the library, not ibus-dev's .so link and headers).
ifeq ($(ARCH),x86_64)
INPUT_BIN := $(LIVE_EXTRA)/usr/bin/myos-input
endif
$(LIVE_EXTRA)/usr/bin/myos-input: live/myos-input/myos-input.c live/common/menu_bubble.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Ilive/common -Wno-deprecated-declarations \
	  -c $< -o $(BUILD)/live/myos-input.o
	$(call LIVE_LINK,$(BUILD)/live/myos-input.o,-l:libibus-1.0.so.5 -lgtk-3 -lgdk-3 -lgio-2.0 \
	  -lgobject-2.0 -lglib-2.0 -lpango-1.0 -lpangocairo-1.0 -lcairo -lX11)
# Dimming, then the screen off, when nobody uses it (live/myos-idle): how
# soon follows the battery slider and the charger.
IDLE_BIN := $(LIVE_EXTRA)/usr/bin/myos-idle
$(IDLE_BIN): live/myos-idle/myos-idle.c live/common/powerconf.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Ilive/common -c $< -o $(BUILD)/live/myos-idle.o
	$(call LIVE_LINK,$(BUILD)/live/myos-idle.o,-lX11 -lXext)
# Sleep (docs/SLEEP-PLAN.md): myos-powerd, root, from boot, turns the lid,
# the power button and the Start menu's Sleep into /sys/power/state;
# myos-lock is the lock screen it asks for first (through myos-idle).
POWERD_BIN := $(LIVE_EXTRA)/usr/libexec/myos-powerd
$(POWERD_BIN): live/myos-powerd/myos-powerd.c live/common/powerconf.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Ilive/common -c $< -o $(BUILD)/live/myos-powerd.o
	$(call LIVE_LINK,$(BUILD)/live/myos-powerd.o,)
LOCK_BIN := $(LIVE_EXTRA)/usr/bin/myos-lock
$(LOCK_BIN): live/myos-lock/myos-lock.c live/common/powerconf.h live/common/xpic.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -Ilive/common -c $< -o $(BUILD)/live/myos-lock.o
	$(call LIVE_LINK,$(BUILD)/live/myos-lock.o,-lX11 -lXft -lImlib2 -lpam)
# power_supply.h on the build machine: a made-up /sys, its times and health.
.PHONY: test-power-supply
test-power-supply:
	@mkdir -p $(BUILD)/host
	cc -std=c11 -Wall -Wextra -Werror -Wno-unused-result live/myos-battery/test-power-supply.c \
	  -o $(BUILD)/host/test-power-supply
	$(BUILD)/host/test-power-supply
# Force Quit (live/myos-forcequit): kill -9 a program that stopped
# answering; Ctrl+Shift+Esc (etc/icewm/keys) and All Programs → System Tools.
FORCEQUIT_BIN := $(LIVE_EXTRA)/usr/bin/myos-forcequit
$(FORCEQUIT_BIN): live/myos-forcequit/myos-forcequit.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Wno-deprecated-declarations \
	  -c $< -o $(BUILD)/live/myos-forcequit.o
	$(call LIVE_LINK,$(BUILD)/live/myos-forcequit.o,-lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lX11)
# An Open panel for shell scripts (live/myos-choose): myos-mplayer asks
# which file to play with it when started from the menu.
CHOOSE_BIN := $(LIVE_EXTRA)/usr/bin/myos-choose-file
$(CHOOSE_BIN): live/myos-choose/myos-choose-file.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Wno-deprecated-declarations \
	  -c $< -o $(BUILD)/live/myos-choose-file.o
	$(call LIVE_LINK,$(BUILD)/live/myos-choose-file.o,-lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0)
# Player (live/myos-player): QuickTime Player's look over mplayer, which
# plays in slave mode inside its window. Menus, My Files and the desktop
# open films and music with it (usr/bin/myos-mplayer).
PLAYER_BIN := $(LIVE_EXTRA)/usr/bin/myos-player
$(PLAYER_BIN): live/myos-player/myos-player.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Wno-deprecated-declarations \
	  -c $< -o $(BUILD)/live/myos-player.o
	$(call LIVE_LINK,$(BUILD)/live/myos-player.o,-lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lcairo -lX11 -lm)
# Preview (live/myos-preview): the Mac's Preview — PDFs through poppler
# (qpdf, run as a program, rewrites their pages), pictures through
# gdk-pixbuf's loaders, and TIFF, JPEG 2000, OpenEXR, Photoshop and HEIC/HEIF
# through libheif (shared heif-pixbuf.c with My Files). /usr/bin/preview is the short name.
PREVIEW_BIN  := $(LIVE_EXTRA)/usr/bin/myos-preview
PREVIEW_SRC  := $(wildcard live/myos-preview/*.c)
PREVIEW_OBJS := $(PREVIEW_SRC:live/myos-preview/%.c=$(BUILD)/live/preview/%.o)
$(BUILD)/live/preview/%.o: live/myos-preview/%.c live/myos-preview/preview.h live/myos-settings/scale.h \
                           | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) $(addprefix -isystem $(LIVE_SYSROOT)/usr/include/,poppler/glib \
	  openjpeg-2.5) -Wno-deprecated-declarations -c $< -o $@
$(PREVIEW_BIN): $(PREVIEW_OBJS) $(HEIF_PIXBUF_OBJ) live/myos-preview/preview.desktop
	@mkdir -p $(dir $@) $(LIVE_EXTRA)/usr/share/applications
	$(call LIVE_LINK,$(PREVIEW_OBJS) $(HEIF_PIXBUF_OBJ),-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lpoppler-glib -lgio-2.0 \
	  -lgobject-2.0 -lglib-2.0 -lpangocairo-1.0 -lpango-1.0 -lcairo -ltiff -lopenjp2 -lheif \
	  -lOpenEXR-3_1 -lX11 -lm)
	ln -sf myos-preview $(dir $@)preview
	install -m 644 live/myos-preview/preview.desktop $(LIVE_EXTRA)/usr/share/applications/
	install -m 644 live/overlay/usr/share/applications/mimeapps.list $(LIVE_EXTRA)/usr/share/applications/
$(XSETTINGS_BIN): live/myos-settings/myos-xsettings.c live/myos-settings/scale.h | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -c $< -o $(BUILD)/live/myos-xsettings.o
	$(call LIVE_LINK,$(BUILD)/live/myos-xsettings.o,-lX11 $(LIVE_SYSROOT)/usr/lib/libXi.so.6)
# /sbin/init's first step: /dev/console to the serial port (TIOCCONS), so
# the boot screen shows the steps and "MyOS is up." and nothing else.
CONSOLE_BIN := $(LIVE_EXTRA)/usr/libexec/myos-console
$(CONSOLE_BIN): live/myos-console/myos-console.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -c $< -o $(BUILD)/live/myos-console.o
	$(call LIVE_LINK,$(BUILD)/live/myos-console.o,)
# Settings' "Add User…": checks an administrator's password with PAM and
# writes the account files. Setuid root, as passwd is.
ADDUSER_BIN := $(LIVE_EXTRA)/usr/libexec/myos-adduser
$(ADDUSER_BIN): live/myos-settings/myos-adduser.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -c $< -o $(BUILD)/live/myos-adduser.o
	$(call LIVE_LINK,$(BUILD)/live/myos-adduser.o,-lpam)
	chmod 4755 $@

# Settings › Kernel's Save: checks an administrator's password and the
# config, and writes the myos-config partition. Setuid root, as myos-adduser.
KCONFIG_BIN := $(LIVE_EXTRA)/usr/libexec/myos-kconfig
$(KCONFIG_BIN): live/myos-settings/myos-kconfig.c live/myos-settings/kconfig.h $(BUILD)/live/kconfig.o \
                | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@) $(BUILD)/live
	$(CC) $(LOGIN_CFLAGS) -c $< -o $(BUILD)/live/myos-kconfig.o
	$(call LIVE_LINK,$(BUILD)/live/myos-kconfig.o $(BUILD)/live/kconfig.o,-lpam)
	chmod 4755 $@

# The installer (live/myos-install): GTK + --text writes a plan; the
# setuid helper is the only process that may write a disk.
INSTALL_BIN := $(LIVE_EXTRA)/usr/bin/myos-install
HELPER_BIN  := $(LIVE_EXTRA)/usr/libexec/myos-install-helper
INSTALL_COMMON := live/myos-install/plan.c live/myos-install/disks.c \
                  live/myos-install/plan.h live/myos-install/disks.h
$(BUILD)/live/install/%.o: live/myos-install/%.c $(INSTALL_COMMON) | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(CC) $(LOGIN_CFLAGS) $(GTK_CFLAGS) -Ilive/myos-install -Ilive/myos-settings \
	  -Wno-deprecated-declarations -Wno-format-truncation -c $< -o $@
# The time zone page's map (made by live/tools/make-worldmap.py).
$(BUILD)/live/install/myos-install.o: live/myos-install/worldmap.h
$(HELPER_BIN): $(BUILD)/live/install/plan.o $(BUILD)/live/install/disks.o \
               $(BUILD)/live/install/myos-install-helper.o
	@mkdir -p $(dir $@)
	$(call LIVE_LINK,$(BUILD)/live/install/plan.o $(BUILD)/live/install/disks.o \
	  $(BUILD)/live/install/myos-install-helper.o,)
	chmod 4755 $@
# The FAT32 writer the helper makes the new EFI partition with (MyOS's own
# FAT driver only reads; the live disk has no mkfs.vfat).
MKESP_BIN := $(LIVE_EXTRA)/usr/libexec/myos-mkesp
$(MKESP_BIN): live/myos-install/myos-mkesp
	@mkdir -p $(dir $@)
	install -m 755 $< $@
# The upgrade's file list (checksums) and this build's version.
MANIFEST_BIN := $(LIVE_EXTRA)/usr/libexec/myos-manifest
$(MANIFEST_BIN): live/myos-install/myos-manifest
	@mkdir -p $(dir $@)
	install -m 755 $< $@
RELEASE_FILE := $(LIVE_EXTRA)/usr/lib/myos/release
$(RELEASE_FILE): $(BUILD)/kernel-version live/tools/write-release.py \
                 $(wildcard $(BUILD)/BOOTX64.EFI) arch/i386/boot/efi/vga-font.bin
	@mkdir -p $(dir $@)
	python3 live/tools/write-release.py --kernel-version $(BUILD)/kernel-version \
	  --arch $(ARCH) --out $@ \
	  $(if $(wildcard $(BUILD)/BOOTX64.EFI),--bootx64 $(BUILD)/BOOTX64.EFI) \
	  --font arch/i386/boot/efi/vga-font.bin
$(INSTALL_BIN): $(BUILD)/live/install/plan.o $(BUILD)/live/install/disks.o \
                $(BUILD)/live/install/myos-install.o live/overlay/usr/share/applications/myos-install.desktop
	@mkdir -p $(dir $@) $(LIVE_EXTRA)/usr/share/applications
	$(call LIVE_LINK,$(BUILD)/live/install/plan.o $(BUILD)/live/install/disks.o \
	  $(BUILD)/live/install/myos-install.o,-lgtk-3 -lgdk-3 -lgdk_pixbuf-2.0 -lgobject-2.0 \
	  -lglib-2.0 -lgio-2.0 -lpango-1.0 -lpangocairo-1.0 -lcairo -lX11 -lm)
	install -m 644 live/overlay/usr/share/applications/myos-install.desktop \
	  $(LIVE_EXTRA)/usr/share/applications/

# WritePerfect: Rodger's WordPerfect 5.1 work-alike, a separate project
# built from its own tree (WP_DIR) into /usr/lib/writeperfect, with its
# grammar checker (languagetool-native, C++) and the checker's English
# rules. /usr/bin/wp ($(WP_DIR)/myos/wp: WritePerfect's own files for MyOS,
# all rights reserved, so they live in its repo, not here) starts it.
#
# The C++ is compiled by clang, not i686-elf-g++: the bare-metal target
# makes size_t `unsigned long`, Linux i386 `unsigned int`, and C++ puts the
# type in every symbol's name (operator new is _Znwm there, _Znwj in
# Alpine's libstdc++), so nothing would link. clang targets Linux exactly;
# GNU ld from i686-elf links, with Alpine's own crtbegin/crtend and libgcc.
WP_DIR   ?= ../wp51/wp51c
WP_LT    := $(WP_DIR)/../languagetool-native
WP_OUT   := $(BUILD)/live/wp
WP_BIN   := $(LIVE_EXTRA)/usr/lib/writeperfect/wp
# Looked up when used ("=", not ":="): on a clean build the sysroot is
# unpacked by this same make run, after the Makefile has been read.
WP_GCC    = $(firstword $(wildcard $(LIVE_SYSROOT)/usr/lib/gcc/$(ALPINE_TRIPLET)/*))
WP_CXXV   = $(notdir $(firstword $(wildcard $(LIVE_SYSROOT)/usr/include/c++/*)))
LIVE_CLANG := clang --target=$(ALPINE_TRIPLET) $(LIVE_MARCH) --sysroot=$(abspath $(LIVE_SYSROOT)) -O2 -MMD -MP
# agent.c calls select() without <sys/select.h>; glibc brings it in through
# other headers, musl doesn't.
WP_CFLAGS  := -std=c11 -D_DEFAULT_SOURCE -w -include sys/select.h -I$(WP_DIR)/src -I$(WP_LT)/engine/include \
              -I$(WP_DIR)/third_party/quickjs-ng $(GTK_CFLAGS)
WP_CXXFLAGS = -std=c++17 -w -nostdinc++ -isystem $(LIVE_SYSROOT)/usr/include/c++/$(WP_CXXV) \
               -isystem $(LIVE_SYSROOT)/usr/include/c++/$(WP_CXXV)/$(ALPINE_TRIPLET) \
               -isystem $(LIVE_SYSROOT)/usr/include/libxml2 -I$(WP_LT)/engine/include \
               -I$(WP_LT)/morfologik/include -DPCRE2_CODE_UNIT_WIDTH=8 \
               '-DLT_RESOURCE_DIR="/usr/share/writeperfect/languagetool"'
# The same files as WritePerfect's linux/Makefile, with the terminal's
# plugin pane (tplug, tmods: QuickJS runs its pages).
WP_C   := doc wpd view cmd files screen iofmt font agent grammar clip extra extra_cmd zh_ai \
          tplug tmods tsave main gui_gtk hanzi_stroke plugin_host
WP_CPP := utf16 StringTools Regex ResourceDataBroker core_types Rule Language JLanguageTool \
          WordTokenizer EnglishWordTokenizer SRXSentenceTokenizer morfologik EnglishTagger \
          XmlRuleDisambiguator EnglishChunker EnglishSynthesizer RuleFilters rules_builtin \
          rules_en PatternRuleLoader lt_c_api
WP_OBJS := $(WP_C:%=$(WP_OUT)/%.o) $(WP_CPP:%=$(WP_OUT)/lt/%.o) $(WP_OUT)/quickjs.o
-include $(WP_OBJS:.o=.d)

$(WP_OUT)/%.o: $(WP_DIR)/src/%.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(LIVE_CLANG) $(WP_CFLAGS) -c $< -o $@
$(WP_OUT)/%.o: $(WP_DIR)/linux/%.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(LIVE_CLANG) $(WP_CFLAGS) -c $< -o $@
$(WP_OUT)/quickjs.o: $(WP_DIR)/third_party/quickjs-ng/quickjs-amalgam.c | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(LIVE_CLANG) -std=gnu11 -w -c $< -o $@
$(WP_OUT)/lt/%.o: $(WP_LT)/engine/src/%.cpp | $(LIVE_SYSROOT)/.done
	@mkdir -p $(dir $@)
	$(LIVE_CLANG) $(WP_CXXFLAGS) -c $< -o $@

$(WP_BIN): $(WP_OBJS) $(WP_DIR)/myos/wp $(WP_DIR)/myos/writeperfect \
           $(WP_DIR)/myos/writeperfect.desktop
	@mkdir -p $(dir $@) $(LIVE_EXTRA)/usr/bin $(LIVE_EXTRA)/usr/share/applications
	$(CC) -nostdlib -s -Wl,-z,noexecstack -Wl,--eh-frame-hdr -Wl,--as-needed \
	  -Wl,-dynamic-linker,$(MUSL_LDSO) \
	  -Wl,-rpath-link,$(LIVE_SYSROOT)/usr/lib:$(LIVE_SYSROOT)/lib \
	  $(LIVE_SYSROOT)/usr/lib/crt1.o $(LIVE_SYSROOT)/usr/lib/crti.o $(WP_GCC)/crtbegin.o \
	  $(WP_OBJS) -L$(LIVE_SYSROOT)/usr/lib -L$(LIVE_SYSROOT)/lib -L$(WP_GCC) \
	  -lgtk-3 -lgdk-3 -lgobject-2.0 -lglib-2.0 -lgio-2.0 -lpango-1.0 -lpangocairo-1.0 -lcairo -lgdk_pixbuf-2.0 \
	  $(if $(wildcard $(LIVE_SYSROOT)/usr/lib/libwebkit2gtk-4.1.so*),-lwebkit2gtk-4.1 -ljavascriptcoregtk-4.1,) \
	  -lncursesw -lz -lxml2 -lpcre2-8 -licuuc -licui18n -lstdc++ -lm -lc -lgcc -lgcc_s \
	  $(WP_GCC)/crtend.o $(LIVE_SYSROOT)/usr/lib/crtn.o -o $@
	install -m 755 $(WP_DIR)/myos/wp $(WP_DIR)/myos/writeperfect $(LIVE_EXTRA)/usr/bin/
	install -m 644 $(WP_DIR)/myos/writeperfect.desktop $(LIVE_EXTRA)/usr/share/applications/
	@mkdir -p $(LIVE_EXTRA)/usr/share/writeperfect
	rsync -a --delete --exclude '*.class' --exclude '*.java' \
	  $(WP_LT)/resources/ $(LIVE_EXTRA)/usr/share/writeperfect/languagetool/
	# Help and samples: the GTK window's Help menu and Sample docs, as on the Mac.
	rsync -a --delete --exclude 'build_*' --exclude '*.c' \
	  $(WP_DIR)/help/ $(LIVE_EXTRA)/usr/share/writeperfect/help/
	rsync -a --delete --exclude 'build_*' --exclude '*.c' \
	  $(WP_DIR)/samples/ $(LIVE_EXTRA)/usr/share/writeperfect/samples/
	# HTML reveal plugins: ZHwriter, Thesaurus, Writer, Reveal Codes.
	rsync -a --delete --exclude '*.LICENSE' \
	  $(WP_DIR)/plugins/ $(LIVE_EXTRA)/usr/share/writeperfect/plugins/

# Built only where WP_DIR is: MyOS's own source on the live disk has no
# WritePerfect tree, and the rest of the disk builds without it.
ifeq ($(wildcard $(WP_DIR)/src/main.c),)
WP_BIN :=
endif

# The firmware drivers load at run time (tools/fetch-firmware.sh, pinned
# linux-firmware files) into the image's /lib/firmware: x86_64 only, for
# iwlwifi.
ifeq ($(ARCH),x86_64)
  FW_STAMP := $(BUILD)/live/firmware.stamp
endif
$(FW_STAMP): tools/fetch-firmware.sh
	tools/fetch-firmware.sh $(LIVE_EXTRA)/lib/firmware
	@mkdir -p $(dir $@) && touch $@

LIVE_THEME := $(LIVE_EXTRA)/.theme
$(LIVE_THEME): live/theme/make-live-theme.py live/assets/themes/golden-hour/make-theme.py \
               $(wildcard live/assets/themes/golden-hour/icons/svg/*.svg) \
               $(wildcard $(WP_DIR)/myos/writeperfect.svg) live/myos-preview/preview.svg
	LIVE_EXTRA=$(LIVE_EXTRA) WP_DIR=$(WP_DIR) python3 live/theme/make-live-theme.py
	@touch $@

live-theme: $(LIVE_THEME)
live-extra: $(FW_STAMP) $(LIVE_THEME) $(LOGIN_BIN) $(CONSOLE_BIN) $(MENU_BIN) $(SETTINGS_BIN) $(XSETTINGS_BIN) $(ADDUSER_BIN) $(KCONFIG_BIN) $(INSTALL_BIN) $(HELPER_BIN) $(MKESP_BIN) $(MANIFEST_BIN) $(RELEASE_FILE) $(FILES_BIN) $(PREVIEW_BIN) $(WIFI_BIN) $(DESKTOP_BIN) $(WP_BIN) $(RM_BIN) $(TRASH_BIN) $(VOLUME_BIN) $(BATTERY_BIN) $(IDLE_BIN) $(POWERD_BIN) $(LOCK_BIN) $(INPUT_BIN) $(FORCEQUIT_BIN) $(CHOOSE_BIN) $(PLAYER_BIN) $(WM_BIN) $(MISSION_BIN)

# The live disk: like `make hd`, but myos-root holds the live userland
# (live/tools/mklivehd.sh: owners, setuid bits and case-only names kept).
# Every overlay file is a prerequisite. Names may have spaces ("My Files.desktop"
# in the Programs menu): find's output is escaped so make keeps each one whole.
OVERLAY_DEPS := $(shell find live/overlay live/overlay.local -type f 2>/dev/null | sed 's/ /\\ /g')
$(LIVE_TAR): $(FW_STAMP) $(LIVE_THEME) $(LOGIN_BIN) $(CONSOLE_BIN) $(MENU_BIN) $(SETTINGS_BIN) $(XSETTINGS_BIN) $(ADDUSER_BIN) $(KCONFIG_BIN) $(INSTALL_BIN) $(HELPER_BIN) $(MKESP_BIN) $(MANIFEST_BIN) $(RELEASE_FILE) $(FILES_BIN) $(PREVIEW_BIN) $(WIFI_BIN) $(DESKTOP_BIN) $(WP_BIN) $(RM_BIN) $(TRASH_BIN) $(VOLUME_BIN) $(BATTERY_BIN) $(IDLE_BIN) $(POWERD_BIN) $(LOCK_BIN) $(INPUT_BIN) $(FORCEQUIT_BIN) $(CHOOSE_BIN) $(PLAYER_BIN) $(WM_BIN) $(MISSION_BIN) $(LIVE_LOCK) live/tools/fetch-live.py $(OVERLAY_DEPS)
	python3 live/tools/fetch-live.py --arch $(ALPINE_ARCH)
ifeq ($(ARCH),x86_64)
$(LIVE_HD): $(LIVE_TAR) live/tools/mklivehd.sh
	E2FS=$(E2FS) live/tools/mklivehd.sh - $(LIVE_TAR) $@ $(LIVE_MB)
else
$(LIVE_HD): $(IMG) $(LIVE_TAR) live/tools/mklivehd.sh tools/mbr-partition.sh
	E2FS=$(E2FS) live/tools/mklivehd.sh $(IMG) $(LIVE_TAR) $@ $(LIVE_MB)
endif
live-hd: $(LIVE_HD)

# RAM login for a UEFI stick when the kernel cannot see the ext2 root.
# The Pentium floppy initrd stays BusyBox. WritePerfect is left on disk.
$(LIVE_LOGIN_TAR): $(FW_STAMP) $(LIVE_THEME) $(LOGIN_BIN) $(CONSOLE_BIN) $(MENU_BIN) $(MISSION_BIN) $(XSETTINGS_BIN) $(WIFI_BIN) \
                   $(RM_BIN) $(LIVE_LOCK) live/tools/fetch-live.py \
                   $(OVERLAY_DEPS)
	python3 live/tools/fetch-live.py --arch $(ALPINE_ARCH) --login-root
	@tar -tf $@ | grep -q '^usr/bin/myos-login$$' \
	  || { echo "login-root.tar has no myos-login"; exit 1; }
$(LIVE_LOGIN_INITRD): $(LIVE_LOGIN_TAR) live/tools/tar2cpio.py live/ram-init
	python3 live/tools/tar2cpio.py $< $@ --add init=live/ram-init
	@test $$(wc -c < $@) -lt $$((384 * 1024 * 1024)) \
	  || { echo "login initrd is over 384 MiB"; exit 1; }
live-login-initrd: $(LIVE_LOGIN_INITRD)

# The live disk in UTM, as its own machine (its own UUID).
UTM_LIVE_VM := dist/MyOS live.utm
# A UEFI machine for trying the installer the way a laptop does it: the live
# system as a USB stick on UTM's USB 3 bus, an empty NVMe disk to install on
# (tools/mkutm.sh --usb-stick). UTM_INSTALL_VM picks where it goes.
UTM_INSTALL_VM ?= dist/MyOS UEFI Install.utm
utm-install: $(LIVE_UEFI)
	@[ "$(ARCH)" = x86_64 ] || { echo "utm-install: make ARCH=x86_64 utm-install"; exit 1; }
	tools/mkutm.sh --x86_64 --usb-stick $(LIVE_UEFI) "$(UTM_INSTALL_VM)"

utm-live: $(LIVE_HD)
	@mkdir -p dist
	tools/mkutm.sh --installed --replace-disk --memory 1024 $(LIVE_HD) "$(UTM_LIVE_VM)" 5C81D2E7-0B4F-4A93-8E6D-71F3A9C2B540

run-live: $(LIVE_HD)
	$(QEMU) -m 1024M -cpu pentium3,+sse2 -drive format=raw,file=$(LIVE_HD),index=0,media=disk \
	  $(DISPLAY_OPTS) -serial stdio
