# Linuxemu v0.1.3

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This patch release targets
the pinned Alpine Linux 3.24.2 armhf minirootfs.

## Install or upgrade on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.3/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

## Fix since v0.1.2

The installed Alpine environment now includes Bash and uses `/bin/bash` as its
interactive and command shell. BusyBox `sh` emitted an `ESC[6n` cursor-position
query that Term49 displayed as question marks. Bash avoids that query without
forcing `TERM=ansi`, so nano and other full-screen programs retain the normal
terminal colors and formatting available through `xterm-256color`.

The installer sets the root account shell and `SHELL` environment variable to
`/bin/bash`, verifies Bash during its staged smoke test, and preserves the
previous installation for `linuxemu-rollback`.

Read `linuxemu-compatibility-0.1.3.md` for the supported surface and
`SHA256SUMS` before using the attached files.
