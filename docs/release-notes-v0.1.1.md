# Linuxemu v0.1.1

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This patch release targets
the pinned Alpine Linux 3.24.2 armhf minirootfs.

## Install or upgrade on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.1/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

The installer verifies both downloads, smoke-tests a staged installation, and
adds `linuxemu` and `alpinx` to the login `PATH`. Upgrading preserves the prior
installation for `linuxemu-rollback`.

## Fix since v0.1.0

The interactive profile no longer aliases every apk command to put
`--no-chown` before the subcommand. That placement caused read-only commands
such as `apk info` to fail. Read-only apk commands now work normally; install
packages with `apk add --no-chown PACKAGE`.

## Verified release

- Host: ARMv7 BlackBerry 10 device reporting QNX 8.0.0 with the BB10 10.3.1
  target runtime.
- Guest: Alpine 3.24.2 armhf with musl 1.2.6 and BusyBox 1.37.
- Full regression: all 14 device suites passed for the runtime binary.
- Installer: public GitHub download, upgrade, command execution, interactive
  greeting, `apk info`, rollback preservation, and checksum verification.

Read `linuxemu-compatibility-0.1.1.md` for the supported surface and
`SHA256SUMS` before using the attached files.
