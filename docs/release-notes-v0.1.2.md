# Linuxemu v0.1.2

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This patch release targets
the pinned Alpine Linux 3.24.2 armhf minirootfs.

## Install or upgrade on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.2/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

## Fix since v0.1.1

BusyBox emits an ANSI `ESC[6n` cursor-position query after its interactive
prompt. Term49 displays that unsupported query as a row of question marks.
Linuxemu now suppresses only this query on tty output when the QNX terminal
reports `TERM=ansi`. Normal terminal output, input, and other escape sequences
are unchanged.

The v0.1.1 correction for read-only apk commands is retained. Install packages
with `apk add --no-chown PACKAGE`.

## Verification

- The captured Term49 prompt bytes identified `ESC[6n` immediately after
  `alpinx:~$ `.
- The native, Linux guest, terminal, application, and installer regression
  suites remain the release gates.
- Public installation preserves the previous version for
  `linuxemu-rollback`.

Read `linuxemu-compatibility-0.1.2.md` for the supported surface and
`SHA256SUMS` before using the attached files.
