# Linuxemu v0.1.6

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This release makes tmux
work, adds a Linux-style descriptor table, fixes several QNX behaviours that
broke Git and `rm -r`, and adds tools for measuring compatibility. It keeps
the v0.1.5 installer fix and the Bash-default environment.

## Install or upgrade on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.6/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

## Changes since v0.1.5

### Terminals and tmux

- tmux 3.7c works, attached as well as detached, with
  `TERM=xterm-256color`, in Term49 and over SSH. QNX dropped `SCM_RIGHTS`
  messages, and still reported success, when the receiving socket was full.
  Every AF_UNIX stream socket now gets a 256 KiB receive buffer.
- Linux `/dev/ptmx` and `/dev/pts/N` are backed by the QNX PTY pool,
  including PTY allocation, unlock, controlling-terminal setup and `forkpty`.
- Terminal descriptors appear in `/proc/self/fd/N` as `/dev/pts/N`.

### UNIX sockets

- Linux pathname UNIX socket addresses map to paths inside the rootfs, and
  returned addresses map back.
- `SCM_RIGHTS` descriptor passing, including `MSG_CMSG_CLOEXEC`, and
  `SO_PEERCRED` peer credentials.

### Descriptors and processes

- The guest has its own descriptor table. `RLIMIT_NOFILE` is virtual (936 on
  the Passport), running out returns `EMFILE`, and descriptors Linuxemu uses
  internally are out of the guest's reach.
- Descriptor flags and records carry across `fork` and `execve`.
- `fork` is retried when QNX fails it with `EBADF` while another thread closes
  a descriptor. This was the cause of intermittent Git HTTPS fetch failures.
- `fcntl(F_SETFD)` and `fcntl(F_SETFL)` return 0, as on Linux, instead of the
  previous flags.
- Directory reads take a snapshot, so deleting entries while listing
  (`rm -r`, `find -delete`) no longer leaves files behind.

### Loader and system calls

- `mmap` accepts `MAP_NORESERVE`.
- Thumb-2 TLS reads and `svc` are patched in statically loaded binaries that
  carry a symbol table.
- New calls: `personality`, `prctl` thread names, `sched_getscheduler`, and
  `rt_tgsigqueueinfo` to the calling thread.

### Measuring compatibility

- `LINUXEMU_DIAGNOSTICS=<file>` makes every guest process log the unsupported
  calls it made when it exits, `exec`s or dies from a signal.
- `scripts/run-corpus.sh` scores a pinned set of eleven Alpine apps. Seven
  are compatible: BusyBox, apk, Python, Git, SQLite, Vim, and GCC with make.
  nginx and Redis need `epoll`, inotify-tools needs `inotify`, and Node.js
  stops at start-up. See `docs/corpus-results.md`.

## Known issues

- Default-speed `cmatrix` freezes the Term49 renderer. Use
  `cmatrix -s -u 10`.
- Python start-up occasionally faults at the same guest address (about one
  run in several on a long-running device session).

## Validation

The full 14-suite device regression passes with 0 failures on a Passport
(QNX 8.0.0) with this exact core binary. The application corpus scores 7 of
11, with the stages listed in `docs/corpus-results.md`.

Read `linuxemu-compatibility-0.1.6.md` for the supported surface and check
`SHA256SUMS` before using the attached files.
