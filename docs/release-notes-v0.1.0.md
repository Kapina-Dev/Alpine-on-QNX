# Linuxemu v0.1.0

> Superseded by v0.1.1. This release's interactive profile puts
> `--no-chown` before every apk subcommand, which breaks commands such as
> `apk info`. Upgrade to v0.1.1.

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This first supported
release targets the pinned Alpine Linux 3.24.2 armhf minirootfs.

## Install on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.0/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

The installer verifies both downloads, smoke-tests a staged installation, and
adds `linuxemu` and `alpinx` to the login `PATH`. It also provides
`linuxemu-rollback` and `linuxemu-uninstall`.

## Verified release

- Host: ARMv7 BlackBerry 10 device reporting QNX 8.0.0 with the BB10 10.3.1
  target runtime.
- Guest: Alpine 3.24.2 armhf with musl 1.2.6 and BusyBox 1.37.
- Applications: apk, nano, OpenSSL-backed HTTPS, Python 3.14.7, SQLite, and
  Git 2.54.0 workflows.
- Regression: all 14 device suites passed, with the resolver and package world
  restored and the 16-package baseline retained.
- Installer: clean install, interactive greeting, command execution, upgrade,
  rollback, uninstall, and checksum-failure atomicity passed on the device.

## Main limits

General Thumb guest syscall scanning, priority-inheritance and cross-process
futexes, socket ancillary data, epoll, graphics, service management, and
arbitrary device ioctls are outside this release. Package installation is
unprivileged and package installation should use `apk add --no-chown PACKAGE`.

Read `linuxemu-compatibility-0.1.0.md` for the complete supported surface and
`SHA256SUMS` before using the attached files.
