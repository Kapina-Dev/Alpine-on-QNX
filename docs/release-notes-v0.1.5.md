# Linuxemu v0.1.5

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This installer hotfix
targets the pinned Alpine Linux 3.24.2 armhf minirootfs.

## Install or upgrade on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.5/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

## Fix since v0.1.4

The installer and generated Linuxemu launcher now set the Alpine guest `PATH`
explicitly. Previously, the staged non-login Bash smoke test inherited the QNX
login path. On devices whose QNX path did not include `/bin`, Bash could not
find BusyBox `grep`, the smoke test failed, and the transactional installer
removed its incomplete staging directory.

The smoke test now invokes `/bin/grep` explicitly and verifies that applet is
present. Bash 5.3 remains `/bin/bash`, the default interactive and command
shell, preserving the Term49 fix for the `?????` prompt artifact.

The Linuxemu runtime is otherwise unchanged from v0.1.4, including the
detached-thread self-stack teardown fix and its server regressions.

Read `linuxemu-compatibility-0.1.5.md` for the supported surface and
`SHA256SUMS` before using the attached files.
