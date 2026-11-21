# Linuxemu v0.1.7

Linuxemu runs 32-bit ARM Linux applications directly on a BlackBerry 10 ARM
CPU and translates their Linux system calls to QNX. This release adds Linux's
event interfaces (eventfd, epoll, timerfd, signalfd and inotify), socket
async I/O, and a batch of modern system calls. nginx, Redis and inotify-tools
now run, and libuv's own event-loop tests pass. It also works around the
intermittent Python start-up crash.

## Install or upgrade on the device

BerryCore by sw7ft must already be installed. As the ordinary Term49/SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.7/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

## Changes since v0.1.6

### Apps

- **nginx** serves, times out idle clients, reloads and stops.
- **Redis** serves clients, using epoll.
- **inotify-tools** (`inotifywait`, `inotifywatch`) watches directories.
- The application corpus now scores 10 of 11 apps; only Node.js still
  fails, at start-up.

### Event interfaces

- **eventfd:** the full object, with semaphore mode, blocking, and sharing
  across `fork`, `execve` and `SCM_RIGHTS`.
- **epoll:** level- and edge-triggered, `EPOLLONESHOT`, `EPOLLRDHUP`, nested
  epoll sets, and safe concurrent `epoll_ctl` and `close`. Sets carry across
  `fork` and `execve`.
- **timerfd:** all the common clocks, with sharing and blocking reads.
- **signalfd:** with a process-wide signal queue, so each signal is delivered
  exactly once, and carried across `fork`, `execve` and `SCM_RIGHTS`.
- **inotify:** built on QNX's native inotify. It supports guest paths and
  `IN_DONT_FOLLOW`, and is carried across `dup`, `fork` and `execve`. Events
  arrive about 250 ms late.
- **Socket async I/O:** `FIOASYNC`, `O_ASYNC`, `F_SETOWN` and `SIGIO` work on
  unix sockets. TCP sockets refuse them, because QNX can then send `SIGIO` to
  unrelated processes and kill them.

### System calls

- New calls: `pipe2`, `dup3` and `accept4` flag cases, `preadv`/`pwritev`
  and `pwritev2`, `sendfile64`, `sendmmsg`/`recvmmsg`, `fallocate`, `statx`,
  `statfs64`, `fchmod`, `faccessat2`, `mknod`, `prlimit64`/rlimits,
  `getrusage`, `sysinfo`, interval timers, priority, affinity and scheduler
  calls, `capget`, more `prctl` options, `membarrier`, and ARM
  `cacheflush`.
- `mremap` works on plain anonymous memory.
- Files are opened with large-file support, and 64-bit offsets, locks and
  stats stay intact.
- `readlink("/proc/self/exe")` returns the running program's guest path.
- Guests can query and set handlers for Linux signals that QNX lacks.

### Fixes

- A signal that arrives just before `poll`, `select` or `epoll_wait` blocks
  now wakes the wait.
- A `SIGTERM` sent right after a child's `execve` is no longer lost.
- After `wait4` reaps a child, its pid is really gone.
- AF_UNIX: `getsockname` and friends return the name the guest bound,
  `connect` to a regular file fails `ECONNREFUSED`, and `fchmod` of a socket
  fails `EOPNOTSUPP` so that libuv falls back to `chmod`.
- **Python start-up crash:** QNX's private file mappings could return wrong
  pages, and musl's loader then crashed in symbol lookup. Writable private
  file mappings are now read in with `pread`.

## Known issues

- Node.js crashes at start-up.
- Default-speed `cmatrix` freezes the Term49 renderer. Use
  `cmatrix -s -u 10`.
- The Python crash fix could not be tested against QNX's bad state, which
  cleared on its own. If Python still faults at start-up at guest address
  `0x60067dc4`, please report it.
- Private writable file mappings are copies, so later writes to the file
  don't show in them. The other limits are in
  `linuxemu-compatibility-0.1.7.md`.

## Validation

The full 16-suite device regression passes with 0 failures on a Passport
(QNX 8.0.0) with this exact core binary. The application corpus scores 10
of 11. Of libuv 1.52.1's 507 tests, 445 pass, 24 are skipped and 38 fail
for gaps planned in later phases. The 158-test event-loop subset passes.
Details are in `docs/corpus-results.md`.

Read `linuxemu-compatibility-0.1.7.md` for the supported surface and check
`SHA256SUMS` before using the attached files.
