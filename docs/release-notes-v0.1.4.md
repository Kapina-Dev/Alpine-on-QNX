# Linuxemu v0.1.4

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This stability release
targets the pinned Alpine Linux 3.24.2 armhf minirootfs.

## Install or upgrade on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.4/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

## Fix since v0.1.3

Detached musl threads can now unmap their own guest stack and exit without
crashing Linuxemu in QNX `MsgSendnc_r`. Linuxemu defers the physical unmap
until execution has returned to the native QNX pthread stack, then completes
the unmap before clearing the Linux child TID and waking waiters.

The regression suite now includes a direct ARM self-stack-unmap fixture and a
Python `ThreadingHTTPServer` workload with 100 sequential connections. The
complete device regression remains green.

Bash 5.3 remains installed as `/bin/bash` and is still the default interactive
and command shell. This preserves the Term49 fix for the `?????` prompt artifact
without reducing `TERM` capabilities or filtering terminal output.

Read `linuxemu-compatibility-0.1.4.md` for the supported surface and
`SHA256SUMS` before using the attached files.
