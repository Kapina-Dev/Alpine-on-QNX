# Linuxemu v0.1.6 compatibility

Linuxemu runs 32-bit ARM Linux applications directly on the ARM CPU of a
BlackBerry 10 phone and translates their Linux system calls to QNX.

## Supported host

- BlackBerry 10 device with an ARMv7 CPU.
- The tested host reports QNX 8.0.0 and the BB10 10.3.1 target runtime.
- Ordinary Term49/SSH user execution; root is not required to run Linuxemu.
- BerryCore by sw7ft is required for the device installer, including its GNU
  Wget with HTTPS support and SHA-256 utilities.

## Supported guest

- Alpine Linux 3.24.2 armhf minirootfs, SHA-256
  `d86af88b58954d8a90c211004f1e61f208a62d1bb5a2f525f0c1cc26408ab92b`.
- Alpine musl 1.2.6 and BusyBox 1.37 from the pinned v3.24 repositories.
- Tested application packages include nano, OpenSSL-backed HTTPS tools,
  Python 3.14.7, SQLite, and Git 2.54.0.

## Verified behavior

The acceptance suite covers static and dynamic ELF loading, files and
directories, contained guest paths, memory mappings, terminals, clocks,
poll/select, IPv4/IPv6/UNIX sockets, DNS, verified HTTPS, apk transactions,
fork/exec/wait and `posix_spawn`, pthreads and the required futex operations,
Linux ARM signal frames and cancellation, detached-thread self-stack cleanup,
runtime executable mappings, Python application workflows including a
100-connection threaded HTTP server, local plus HTTPS Git workflows, pathname
UNIX sockets, `SCM_RIGHTS` descriptor passing (including large nonblocking
frames), peer credentials, and Linux PTY/forkpty workflows backed by QNX
pseudoterminals. An attached tmux 3.7c client with `TERM=xterm-256color`
attaches and exits cleanly, in an SSH pseudo-terminal and in Term49.

The 0.1.6 core adds a Linux-style descriptor table: `RLIMIT_NOFILE` is
virtual (936 on the Passport), descriptor exhaustion returns `EMFILE`,
descriptor records survive `fork` and `execve`, and directory listings stay
complete when entries are deleted mid-walk (`rm -r`, `find -delete`). Set
`LINUXEMU_DIAGNOSTICS=<file>` to log the unsupported calls a guest makes.

Phase 2 adds these Linux calls, each with a direct ARM guest test:

- `rt_sigaction` queries and stores for every signal 1 to 64, and ARM
  `cacheflush`.
- `preadv`, `pwritev`, `preadv2`, `pwritev2`, `sendfile64`, `fallocate`, and
  the `fcntl64` owner, pipe-size and open-file-description lock commands.
  Positional and vector I/O keep 64-bit offsets.
- `statx`, `statfs64`, `fchmod`, `faccessat2`, `chown32` and its relatives,
  `mknod` and `mknodat`.
- `sendmmsg` and `recvmmsg`, direct and through `socketcall`.
- `getrlimit`, `ugetrlimit`, `setrlimit`, `getrusage`, `sysinfo`,
  `setitimer`, `getitimer`, `setpriority`, `getpriority`,
  `sched_getaffinity`, `sched_setaffinity`, `sched_setscheduler`, `capget`,
  `capset`, the `prctl` dumpable, no-new-privs and parent-death-signal
  options, `membarrier`, and `mremap`.
- `pipe2`, `dup3` and `accept4` now have direct tests for flags and
  replaced descriptors. `dup2` and `dup3` are serialized against `close`
  inside one process.

The pinned application corpus scores 7 of 11 apps compatible: BusyBox, apk,
Python, Git, SQLite, Vim, and GCC with make. nginx and Redis need `epoll`,
inotify-tools needs `inotify`, and Node.js stops at start-up on `epoll_create1`. After Phase 2 none of the corpus apps hit ENOSYS for the calls it added.

## Deliberate limits

Limits of the Phase 2 calls:

- `preadv`, `pwritev` and their `*2` forms, and `sendfile64`, are loops over
  `pread`/`pwrite` and `read`/`write`. They are not atomic across iovecs.
  `RWF_HIPRI`, `RWF_NOWAIT` and `RWF_APPEND` are refused.
- Open-file-description locks (`F_OFD_*`) are per-process, not per open file.
  `fcntl` record-lock offsets are still 32-bit.
- Signals 16 and 48 to 64 have no QNX number. Their actions can be queried
  and stored, but sending one (`kill`, `tkill`, `tgkill`, `rt_sigqueueinfo`)
  returns `EINVAL`.
- `membarrier` supports no commands: `QUERY` returns 0 and every other
  command returns `EINVAL`, so libc falls back to its signal-based method.
- `mremap` handles plain anonymous guest memory that was never executable.
  File mappings, executable memory, `MREMAP_FIXED` and
  `MREMAP_DONTUNMAP` return `EINVAL`. Memory that was ever patched is
  refused too.
- Memory that a JIT flips to executable is scanned blindly for `svc #0` and
  TPIDRURO words. Data words that match are rewritten, and about 4,000 flips
  fill the patch table. The fix belongs with Phase 4.
- `statx` returns whole-second timestamps and no birth time. Device numbers
  are a synthetic major/minor split of the QNX device number.
- `ITIMER_VIRTUAL` and `ITIMER_PROF` are unsupported. An armed `ITIMER_REAL`
  is lost across `execve`.
- `setpriority` is emulated inside Linuxemu and has no scheduling effect, as
  QNX's own `getpriority`/`setpriority` are stubs. The parent-death signal
  is stored but never delivered, because QNX has no parent-death
  notification. `sysinfo` reports no load average.
- Files larger than about 2 GiB could not be tested: the device file systems
  (qnx6 and exFAT) cap a file near 2 GiB.

- ARM `svc #0` patching is supported. Thumb `svc` and TLS reads are patched
  only in statically loaded binaries that carry a symbol table, not in
  libraries mapped at run time.
- Priority-inheritance and robust-list futex operations are unsupported.
  Process-shared futexes do not synchronize separate Linuxemu host processes.
- Socket ancillary data other than `SCM_RIGHTS`, unlisted options, arbitrary
  ioctls, `epoll`, `inotify`, graphics, kernel modules, and Linux service management are
  outside the v0.1 surface.
- Linux `/dev/ptmx` and `/dev/pts/N` use the finite QNX `/dev/ptypN` and
  `/dev/ttypN` pool; allocation fails when every native pair is busy.
- Terminal descriptors are exposed through `/proc/self/fd/N` as guest
  `/dev/pts/N` names. Attached and detached tmux workflows pass. Every
  AF_UNIX stream socket gets a 256 KiB receive buffer, because QNX silently
  drops `SCM_RIGHTS` messages sent to a full receiver.
- Default-speed cmatrix and Ctrl-C pass through an SSH pseudo-terminal. The
  physical Term49 session still freezes at that output rate (confirmed on
  the device on 2026-10-14); `cmatrix -s -u 10` remains the conservative
  command.
- `eventfd2` implements the pipe-backed wake descriptor form needed by libcurl,
  rather than general Linux event counters.
- Executable shared file mappings are rejected. Private executable file
  mappings use an anonymous copy.
- Package installation is unprivileged and should use
  `apk add --no-chown PACKAGE`.
- Guest paths remain inside the Alpine rootfs. Host shared-media directories
  are not mounted into the guest.
- The installed interactive environment uses Bash. BusyBox `sh` emits an ANSI
  `ESC[6n` cursor-position query that Term49 displays as question marks; Bash
  avoids that query and preserves the terminal's normal capabilities.

Unsupported system calls return Linux errors. The supported release does not
promise compatibility with arbitrary Linux binaries beyond this tested matrix.
