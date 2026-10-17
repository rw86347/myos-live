# myos-live — MyOS's live and installer disks

The desktop and everything around it that turns the [MyOS kernel](https://github.com/rw86347/MyOS)
into a system you can start from a USB stick and install: the MyOS desktop
programs (login screen, Start menu, files, settings, the installer...), the
overlay of configuration files, the lists of Alpine Linux packages the disk
carries (pinned with their SHA-256s in `packages.lock`), the Golden Hour
theme, and the tools that build and test the images.

Third-party programs (AbiWord, Chromium, GTK...) are not stored here: they
are Alpine packages, fetched and checked at build time.

## Getting it

This repo is checked out *inside* a MyOS checkout, as `live/`:

    git clone https://github.com/rw86347/MyOS.git
    cd MyOS
    git clone https://github.com/rw86347/myos-live.git live

MyOS's Makefile includes `live/live.mk` when it is there, so everything is
built from the MyOS folder:

    make ARCH=x86_64 live-uefi        # the 64-bit UEFI stick image
    make live-hd                      # the 32-bit BIOS live disk
    make ARCH=x86_64 test-install     # install from the stick in QEMU, start the result
    make ARCH=x86_64 utm-install      # a UTM machine to try the installer in

WritePerfect is a separate project with its own repo; the build includes it
when it is checked out at `../wp51/wp51c` (`WP_DIR`), and leaves it out
otherwise.

BSD 2-Clause License (see LICENSE).
