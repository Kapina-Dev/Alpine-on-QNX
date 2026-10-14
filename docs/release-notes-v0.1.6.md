# Linuxemu v0.1.6

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This release expands the
terminal and local IPC surface while retaining the v0.1.5 installer fix and
Bash-default environment.

## Install or upgrade on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.6/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

## Changes since v0.1.5

- Translate full-size, zero-padded Linux pathname UNIX socket addresses into
  contained QNX rootfs paths and translate returned addresses back to Linux.
- Support `SCM_RIGHTS` file-descriptor passing, including
  `MSG_CMSG_CLOEXEC`, and translate QNX peer identity to Linux
  `SO_PEERCRED`.
- Grow QNX's small UNIX-socket send buffer before large nonblocking
  `SCM_RIGHTS` frames. This prevents descriptor loss during tmux's full
  `xterm-256color` terminal-identification handshake.
- Back Linux `/dev/ptmx` and `/dev/pts/N` with the finite QNX legacy PTY pool.
- Translate PTY allocation/unlock queries and Linux controlling-terminal setup
  to QNX `tcsetsid` semantics.
- Add direct pathname-socket, descriptor-passing, peer-credential, and
  forkpty regressions.
- Validate detached tmux 3.7c server startup, client authentication, session
  creation, pane output capture, and clean shutdown, plus an attached
  `TERM=xterm-256color` client over an SSH pseudo-terminal.

Publishing remains gated on a fresh-device rerun of the complete 14-suite
regression. The Phase 0 candidate passes the first 12 suites and the final Git
suite; on the current long-running device session, Python startup faults at the
same guest address with both the installed pre-Phase-0 core and the candidate,
so that failure is not attributed to this socket change.

Read `linuxemu-compatibility-0.1.6.md` for the supported surface and
`SHA256SUMS` before using the attached files.
