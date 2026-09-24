# Device installation

## Prerequisite

Install BerryCore by sw7ft first. Its `wget` and `sha256sum` commands must be
available in the QNX shell `PATH`. Linuxemu installation and use do not require
the root SSH account.

## Install

Download the installer associated with the release tag and execute it:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.0/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
```

Reconnect through SSH, or reload the current login profile:

```sh
. ~/.profile
alpinx
```

The default prefix is `/accounts/1000/shared/misc/linuxemu`. Override it with
`LINUXEMU_PREFIX` or `--prefix`. Override the default guest DNS server with
`DNS_SERVER`, for example:

```sh
DNS_SERVER=192.168.1.1 sh /tmp/install-linuxemu.sh
```

The installer downloads the pinned runtime bundle and official Alpine 3.24.2
armhf minirootfs, verifies their SHA-256 hashes, builds a staged installation,
and runs a guest smoke test before replacing an existing installation.

## Commands

- `alpinx` or `linuxemu` opens an interactive Alpine login shell.
- `linuxemu COMMAND ARG...` runs a guest command.
- `linuxemu -c 'SHELL COMMAND'` runs a guest shell command.
- `linuxemu-rollback` exchanges the current and previous installations.
- `linuxemu-uninstall` removes Linuxemu and its profile entry.

Use `apk --no-chown` inside Alpine because the rootfs is owned by the ordinary
QNX user. The interactive profile supplies an `apk` alias with that option.
