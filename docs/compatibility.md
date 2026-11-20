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
virtual (744 on the Passport since Phase 3 part 1, which reserves 256
host descriptors for the emulator; 936 in the 0.1.6 release), descriptor
exhaustion returns `EMFILE`,
descriptor records survive `fork` and `execve`, and directory listings stay
complete when entries are deleted mid-walk (`rm -r`, `find -delete`). Set
`LINUXEMU_DIAGNOSTICS=<file>` to log the unsupported calls a guest makes.

Phase 2 adds these Linux calls, each with a direct ARM guest test:

- `rt_sigaction` queries and stores for every signal 1 to 64, and ARM
  `cacheflush`.
- `preadv`, `pwritev`, `preadv2`, `pwritev2`, `sendfile64`, `fallocate`,
  and the `fcntl64` pipe-size and open-file-description lock commands. The
  `fcntl64` owner and seal commands are answered with `EINVAL`. Positional
  and vector I/O, and record locks, keep 64-bit offsets, and guest files
  are opened as large files.
- `statx`, `statfs64`, `fchmod`, `faccessat2`, `chown32` and its relatives,
  `mknod` and `mknodat`.
- `sendmmsg` and `recvmmsg`, direct and through `socketcall`.
- `ugetrlimit`, `prlimit64`, `setrlimit`, `getrusage`, `sysinfo`,
  `setitimer`, `getitimer`, `setpriority`, `getpriority`,
  `sched_getaffinity`, `sched_setaffinity`, `sched_setscheduler`, `capget`,
  `capset`, the `prctl` dumpable, no-new-privs and parent-death-signal
  options, `membarrier`, and `mremap`.
- `pipe2`, `dup3` and `accept4` now have direct tests for flags and
  replaced descriptors. `dup2` and `dup3` are serialized against `close`
  inside one process.

Phase 3 part 1 adds the full Linux eventfd and epoll, each covered by the
`run-event-smoke` guest fixtures:

- `eventfd` and `eventfd2` with Linux semantics: a 64-bit counter shared
  through memory, `EFD_SEMAPHORE`, `EFD_NONBLOCK` and `EFD_CLOEXEC`,
  blocking reads and writes (interrupted by signals, restarted with
  `SA_RESTART`), overflow (`EAGAIN`, or a blocking write released by a
  read), the Linux `EINVAL` cases, and `POLLIN`/`POLLOUT` readiness in
  `poll`, `select` and epoll. One eventfd is the same object across `dup`,
  `fork`, `execve` and `SCM_RIGHTS` (to a Linuxemu receiver), and its
  `O_NONBLOCK` flag is shared as on Linux. libcurl's wake descriptor is
  now this eventfd.
- `epoll_create`, `epoll_create1`, `epoll_ctl` (`ADD`, `MOD`, `DEL` with
  Linux's errors, including `EPERM` for regular files and directories and
  `ELOOP` for cycles), `epoll_wait`, `epoll_pwait` with a signal mask, and
  `epoll_pwait2` with a `timespec` timeout. Level-triggered readiness,
  `EPOLLET`, `EPOLLONESHOT`, `EPOLLRDHUP`, `EPOLLEXCLUSIVE` (Linux's
  validation rules) and `EPOLLWAKEUP` (accepted, never reported); user data
  is returned unchanged, and a small `maxevents` rotates fairly between
  ready items. A watched fd that is closed, replaced by `dup2`/`dup3` or
  whose number is reused is dropped, also while another thread waits, and
  `ctl` from another thread wakes a waiter. Nested epoll (depth 5, as on
  Linux), epoll fds in `poll` and `select`, and epoll across `fork` and
  `execve` work. A Looper-style loop (eventfd wake, pipes and sockets,
  timeouts, fds added and removed while waiting) is covered.
- A pipe read end at EOF now reports `POLLHUP` (and `EPOLLHUP`) as on Linux;
  QNX reports only readable.
- `select` and `pselect6` accept descriptors up to 4096 (QNX's own
  `FD_SETSIZE` is 256).

The pinned application corpus scores 7 of 11 apps compatible after Phase 3
part 1: apk, Python, Git, SQLite, Vim, GCC with make, and now Redis.
BusyBox failed its shutdown stage once in that run and passed it in three
reruns (an intermittent watch item). nginx serves nothing because
`ioctl(FIOASYNC)` on its worker channel returns `ENOTTY`, inotify-tools
needs `inotify`, and Node.js gets past `epoll_create1` and then crashes at
start-up (SIGSEGV, address 0). No corpus app hits an unsupported eventfd
or epoll call.

Phase 3 part 2 adds `timerfd` and `signalfd`, each covered by the
`run-event-smoke` guest fixtures (the application corpus was not re-run):

- `timerfd_create`, `timerfd_settime` and `timerfd_gettime` (and the time64
  forms) with Linux semantics: one-shot, periodic and absolute deadlines on
  `CLOCK_REALTIME`, `CLOCK_MONOTONIC`, `CLOCK_BOOTTIME` and the `*_ALARM`
  clocks, an expiration count read as 8 bytes, `TFD_NONBLOCK`,
  `TFD_CLOEXEC`, `TFD_TIMER_ABSTIME`, `TFD_TIMER_CANCEL_ON_SET`,
  `TFD_IOC_SET_TICKS`, blocking reads (interrupted by signals, restarted
  with `SA_RESTART`, ended by a close from another thread), readiness in
  `poll`, `select` and epoll, and the same timer across `dup`, `fork`,
  `execve` and `SCM_RIGHTS`. `clock_gettime` accepts the `CLOCK_BOOTTIME`
  family, and the guest `clock_settime` calls exist (they fail `EPERM` for
  an ordinary user).
- `signalfd` and `signalfd4`: 128-byte `signalfd_siginfo` records, standard
  signals coalescing and real-time signals queueing in order, nonblocking
  and blocking reads, several signalfds, mask changes, readiness in `poll`
  and epoll, and a signalfd across `fork`, `execve` and `SCM_RIGHTS`. A
  signal read through a signalfd is not also delivered. A guest
  `sigprocmask` now reaches the host thread's mask, so a blocked signal no
  longer kills the process with its default action. `rt_sigsuspend`,
  `ppoll`, `pselect6` and `epoll_pwait` see signals a signalfd's collector
  took.

Phase 3 part 3 adds `inotify` on QNX's native inotify, covered by the
`run-event-smoke` fixtures `inotify-basic`, `inotify-modes` and
`inotify-share`:

- `inotify_init`, `inotify_init1` (`IN_NONBLOCK`, `IN_CLOEXEC`),
  `inotify_add_watch` and `inotify_rm_watch`, with Linux's
  `struct inotify_event` records. Create, modify, attrib, close-write,
  delete, rename (`MOVED_FROM`/`MOVED_TO` with a shared cookie),
  `DELETE_SELF` and `IGNORED`, `IN_ONESHOT`, `IN_MASK_ADD`, `IN_ONLYDIR`,
  `IN_DONT_FOLLOW`, queue overflow, `FIONREAD`, and Linux's errors where
  QNX differs (a nonblocking read of an empty queue fails `EAGAIN`, a mask
  with no event bits fails `EINVAL`).
- Guest paths: relative watches follow the guest's working directory, and
  guest symlinks resolve inside the rootfs. Bad path or buffer pointers
  fail `EFAULT`.
- Readiness in `poll`, `select`, `ppoll` and epoll (level- and
  edge-triggered), blocking reads interrupted by signals, a burst of 100
  creates from four threads with every event delivered once, and the same
  queue across `dup`, `fork` and `execve`.

The pinned application corpus now scores 9 of 11 apps compatible:
inotify-tools joins apk, BusyBox, GCC with make, Git, Python, Redis,
SQLite and Vim (`docs/corpus-results.md`, 2026-11-20). nginx and Node.js
fail as after part 1.

## Deliberate limits

Limits of the Phase 2 calls:

- `preadv`, `pwritev` and their `*2` forms, and `sendfile64`, are loops over
  `pread`/`pwrite` and `read`/`write`. They are not atomic across iovecs.
  `RWF_NOAPPEND` (which musl's `pwrite` uses) is accepted for positioned
  writes; at the current position of an `O_APPEND` descriptor it is
  refused. `RWF_HIPRI`, `RWF_NOWAIT` and `RWF_APPEND` are refused.
  `pwrite`/`pwritev` on an `O_APPEND` descriptor write at the given offset,
  as QNX does; Linux appends instead.
- Open-file-description locks (`F_OFD_*`) are per-process, not per open
  file. Two threads of one process don't exclude each other, and closing
  any descriptor of the file drops the locks.
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
  TPIDRURO words. Data words that match are rewritten; the fix belongs with
  Phase 4. Repeated flips of the same page no longer fill the patch table.
- `statx` returns whole-second timestamps and no birth time. Device numbers
  are a synthetic major/minor split of the QNX device number.
- `cacheflush` returns `EINVAL` when start equals end, and for a range
  that spans two adjacent mappings.
- `ITIMER_VIRTUAL` and `ITIMER_PROF` are unsupported. An armed `ITIMER_REAL`
  is lost across `execve`.
- `setpriority` is emulated inside Linuxemu and has no scheduling effect, as
  QNX's own `getpriority`/`setpriority` are stubs. The parent-death signal
  is stored but never delivered, because QNX has no parent-death
  notification, and it is lost across `execve`. `sysinfo` reports no load
  average and a process count of 1.
- Where `faccessat2` checks permissions itself (for example with
  `AT_EMPTY_PATH`), it uses the owner and primary group only and ignores
  supplementary groups.
- QNX accepts no finite `RLIMIT_FSIZE` for an ordinary user. Setting one
  returns `EINVAL` and is reported as `rlimit-host-refused`.
- Files past 4 GiB work: guest files are opened with QNX `O_LARGEFILE`. The
  device's qnx6 file system is not sparse, so a write far past the end of a
  file first fills the gap with zeros.

Limits of the Phase 3 part 1 calls (eventfd and epoll):

- **Capacity.** Each process keeps 256 host descriptors hidden for the
  emulator, so the guest's `RLIMIT_NOFILE` is 744 on the Passport (936
  before). An eventfd uses two hidden descriptors and an epoll two (its
  wake channel's write end and a private duplicate of its read end, so a
  wait never depends on the guest's epoll fd number staying open), so a
  process holds about 128 eventfds, fewer with epolls and other hidden
  descriptors. Running out returns `EMFILE`.
- **Signal-wait channels.** A thread that blocks in an eventfd `read` or
  `write`, or in `ppoll`, `pselect6` or `epoll_pwait` with a signal mask,
  gets one signal-wait channel on its first such wait and keeps it until
  it exits: a pipe (two hidden descriptors). About 127 threads can
  therefore be blocked at once (100 are tested), and threads that waited
  once keep their pair while idle, so more threads than that reach
  `EMFILE`. A one-descriptor loopback UDP socket was tried and dropped:
  the signal handler writes the channel, and a stress test that did so
  crashed the Passport's network stack (io-pkt), see `docs/FINDINGS.md`
  2026-11-19.
- **Fork copies an epoll.** Linux shares one epoll between parent and
  child; here the child gets an independent copy of the interest list, and
  a change in one process isn't seen by the other. nginx, Redis and
  libuv create their epoll after forking or exec at once.
- **Registrations follow the fd number.** A watched fd's `dup` that
  outlives the original number doesn't keep the registration: closing the
  original number drops it, where Linux keeps it until the open file is
  fully closed.
- **`EPOLLET` is approximated.** QNX `poll()` is level-triggered. After an
  edge-triggered item is reported, it stays quiet until the guest reads,
  writes, sends, receives or accepts on that fd number, or modifies the
  item. So:
  - new data arriving while the guest hasn't touched the fd gives no new
    edge (programs written for `EPOLLET` drain to `EAGAIN`, which re-arms);
  - any I/O re-arms, so an item can be reported again with no new data
    (a spurious wakeup);
  - I/O on a `dup` of the watched number doesn't re-arm the item;
  - an edge-triggered outer epoll watching an inner epoll re-arms only when
    the inner epoll is read or waited on.
- **`EPOLLEXCLUSIVE`** is validated as Linux does and then treated as a
  plain registration: every waiter may wake, not just one.
- **`EPOLLRDHUP`** is reported for `SOCK_STREAM` sockets only (not
  `SOCK_SEQPACKET`), only once unread data has been read (Linux reports it
  at once), and only when `EPOLLIN` is requested too; an item asking for
  `EPOLLRDHUP` alone sees it only when the peer fully closes.
- **Pipe hangups.** A pipe whose writer closed with data left reports no
  `POLLHUP`/`EPOLLHUP` until the data is read, because QNX gives no sign
  that the writer is gone. A named FIFO that no writer ever opened may
  report `POLLHUP` where Linux blocks (not measured). The write end of a
  pipe whose reader closed gets no `POLLERR`.
- **eventfd writes near the maximum.** A writer waiting in `poll`, `select`
  or epoll for `POLLOUT` on an eventfd whose counter is at its maximum
  (`0xfffffffffffffffe`) isn't woken when a reader drains it; it sees
  `POLLOUT` on its next wait. A blocking `write` itself is released.
- **Closing a blocked eventfd.** If another thread closes an eventfd while
  a `read` blocks on it, the read fails `EBADF`; Linux lets it finish.
- **A killed process mid-update.** A process killed by `SIGKILL` in the few
  instructions between draining an eventfd's readiness pipe and raising it
  again leaves a nonzero counter with an empty pipe. `poll`, `select` and
  epoll check the counter before sleeping, so a Linuxemu poller still sees
  the eventfd as readable.
- **Passing over `SCM_RIGHTS`.**
  - An epoll fd received from another process arrives as a plain pipe,
    not an epoll.
  - An eventfd travels with its hidden descriptors appended to the message.
    A receiver that isn't Linuxemu (a native QNX process) gets those extra
    descriptors as ordinary fds.
  - If the host truncates the message, the receiving Linuxemu can't
    recognise the extra descriptors, so a guest with a large enough control
    buffer receives the sender's hidden pipe write end and shared-memory fd
    as ordinary fds.
  - If rebuilding the eventfd fails on the receiving side, the guest gets
    the bare readiness pipe instead of an eventfd, without an error.
  - More than 253 guest fds in one message returns `EINVAL`, as on Linux.
    Each eventfd adds two hidden descriptors to the host message; how many
    descriptors QNX accepts in one message isn't measured.
  - The `LXRIGHT1` descriptor map that carries the hidden fds is ordinary
    message data, so a guest can forge one. A forged map affects only that
    guest's own message.
- **Nesting and exec.** A process holds at most 256 nested epoll
  registrations (an epoll inside another); more returns `ENOMEM` and is
  reported as `epoll-nest-table-full`. An epoll kept across `execve` (no
  `EPOLL_CLOEXEC`) carries about 21 to 30 items; a longer list arrives
  empty and is reported as `epoll-exec-list-too-long`.
- **Exec records.** Descriptor records travel to the new image in a 32 KB
  environment entry. A record that doesn't fit is left out on its own and
  reported as `exec-fd-record-dropped` (its fd survives `execve` as a plain
  native fd); the other records still travel. Before, one oversized record
  dropped them all.
- `select` and `pselect6` accept descriptor numbers up to 4096 (QNX's own
  `FD_SETSIZE` is 256); Linux bounds them only by `RLIMIT_NOFILE`.

Limits of the Phase 3 part 2 calls (timerfd and signalfd):

- **Clocks.** `CLOCK_BOOTTIME` maps to `CLOCK_MONOTONIC`, and
  `CLOCK_REALTIME_ALARM` and `CLOCK_BOOTTIME_ALARM` map to the plain
  realtime and monotonic clocks. Alarm timers therefore fire only while the
  device is awake and cannot wake it. Whether QNX's monotonic clock runs
  during device sleep is unmeasured.
- **Timers and processes.** A timerfd fires from the timer manager of the
  process that armed it. A fork child or an `SCM_RIGHTS` receiver gets
  `poll`/epoll wake-ups only while that process lives; its blocking `read`
  still counts from the clock. An exec'd image re-arms an inherited timer
  in its own manager and fires it itself. When another process disarms or
  re-arms a shared timerfd, the first process's timer follows at its next
  pulse. Expirations are counted from the clock; a sub-millisecond period
  counts correctly but the wake-up rate is about one per millisecond, and
  pulses arrive about 2.5 ms late at the median (p99 about 5.6 ms).
- **`TFD_TIMER_CANCEL_ON_SET`.** Accepted for absolute `CLOCK_REALTIME`
  timers, where a read fails `ECANCELED` after a clock change; accepted and
  ignored for relative and non-realtime timers, as on Linux. A change is
  detected from `PROCMGR_EVENT_TOD` plus a realtime-minus-monotonic offset
  check. A real clock change could not be produced on the device
  (`clock_settime` fails `EPERM` for the ordinary user), so this is
  verified only with a manually triggered event.
- Relative `CLOCK_REALTIME` timers shift when the clock steps (Linux keeps
  them on the monotonic base). Counts are capped at 2^48-1 per arming. A
  `timerfd_settime` in one process racing a read in another can, rarely,
  let the read add the old arming's uncounted expirations (bounded by what
  it produced) after the reset.
- **signalfd and delivery.** A signal is read through a signalfd when it
  is blocked in every thread, or in the thread that reads. A collector
  thread takes such signals into a process-wide queue; if a thread has the
  signal unblocked, the collector hands it to that thread instead, and
  interrupts threads that have it unblocked, so their blocking calls can
  return `EINTR` (as on Linux; a behaviour change from before part 2).
- **Thread-directed signals.** A signal sent to one thread (`tgkill`) that
  the thread blocks stays in the QNX kernel until that thread reads its
  signalfd; `read` returns it but `poll` and epoll don't show it first,
  and a `read` already blocked when it arrives isn't woken by it: it finds
  the signal at its next one-second re-check (up to 1 s late).
- **Sender details.** The sender pid, code and `sigqueue` value come
  from the QNX siginfo. QNX reports `si_uid` 0 for a non-zero sender, so
  `ssi_uid` is the receiver's real uid in that case. Across `execve`
  pending signals are re-raised once each in the new image: sender details,
  `sigqueue` values and real-time counts are lost, and a signal collected
  between the exec snapshot and the host `execve` is lost. Thread-directed
  signals come back process-directed (QNX `sigpending` can't tell them
  apart), except signals only in the calling thread's queue, which go to
  the main thread.
- **`SCM_RIGHTS`.** A signalfd sent to another Linuxemu process is rebuilt
  there; the receiver reads its own signals (as on Linux) and the mask is
  shared with the sender. A mask change through `signalfd(fd, newmask)`
  reaches other processes at their next read or poll; a process already
  asleep keeps the old set.
- **Capacity.** Each signalfd costs 4 hidden descriptors per process and
  each timerfd 2 (its readiness pipe's write end and its shared-memory
  page), so about 64 signalfds or 128 timerfds fit in the 256 hidden
  descriptors, which eventfd and epoll share too. So do the per-thread
  signal-wait pipes (2 each), made on a thread's first blocking eventfd
  call, `rt_sigsuspend` or masked wait (`ppoll`, `pselect6`,
  `epoll_pwait`). When the range is full, `rt_sigsuspend` falls back to
  the host `sigsuspend` (reported once as `sigsuspend-no-wait-channel`);
  there a signal queued at the very moment the wait starts can be missed
  until the next one. If a fork child can't build its readiness
  channel (hidden range full), `poll`, `select` or epoll on that signalfd
  can sleep until their timeout; this is reported once in
  `LINUXEMU_DIAGNOSTICS`.
- **Spurious readiness.** Within one process the readiness byte can stay
  after a handler or `sigwaitinfo` consumed the signal: `poll` reports
  `POLLIN` and the read then fails `EAGAIN`.
- **Still unimplemented.** `rt_sigpending` and `rt_sigtimedwait` return
  `ENOSYS` (a follow-up; they must include the process queue). The
  stop-default signals (`SIGTSTP`, `SIGTTIN`, `SIGTTOU` at `SIG_DFL`) end a
  masked wait (`sigsuspend`, `ppoll`, ...) with a bare `EINTR`; Linux keeps
  sleeping. The collector's race-path wake-up goes to every thread with the
  signal unblocked and has no `SA_RESTART`, so a spurious `EINTR` is
  possible, rarely.

Limits of the Phase 3 part 3 calls (inotify):

- **Unsupported filesystems.** A watch on a filesystem QNX can't watch
  (`/dev/shmem`, `/pps`, `/proc`, where the native call fails `ENOTTY`)
  returns `EOPNOTSUPP` and is reported in `LINUXEMU_DIAGNOSTICS`, rather
  than a watch that never fires. A guest path can't reach these today: the
  guest's `/dev` maps only `/dev/null` and `/dev/pts/*`, which are device
  nodes (next item), so this mapping is untested from a guest.
- **Device nodes.** A watch on a character or block device returns
  `EOPNOTSUPP` (reported as above) without calling QNX: the native
  `inotify_add_watch` on some ptys (`/dev/ttyp1`, `/dev/ttyp5`) blocks
  until a signal arrives. Linux allows such watches.
- **Late events.** QNX batches inotify events: they arrive about 250 ms
  after the change (median 253 ms, at most 255 ms measured), sometimes
  sooner. A program that waits a fixed short time for an event can miss it.
- **Overflow record first.** When the queue overflows (at 2,048 events, not
  Linux's default 16,384), QNX puts the `IN_Q_OVERFLOW` record (`wd` -1,
  `len` 0) first in the queue, ahead of the queued events; Linux appends it
  at the end. It is passed through as QNX queues it.
- **Extra events.** Passport background processes open files and
  directories after changes, so watches with `IN_OPEN`, `IN_ACCESS` or
  `IN_CLOSE_NOWRITE` see extra events. They are real events and are passed
  through.
- **`IN_MASK_CREATE`** returns `EINVAL` whether or not a watch exists for
  the path, because QNX can't tell whether one does without Linuxemu
  tracking every watch. Linux returns `EEXIST` only for an existing watch.
- **`SCM_RIGHTS`.** `sendmsg` and `sendmmsg` with an inotify fd in
  `SCM_RIGHTS` fail `EOPNOTSUPP` and send nothing: QNX can't pass the fd
  (its receiver fails `EACCES` and loses the whole message). A `sendmmsg`
  returns the count of messages sent before the refused one.
- **`IN_EXCL_UNLINK`** is passed to QNX unchanged and is untested.
- **No `/proc/sys/fs/inotify`.** `max_queued_events`,
  `max_user_instances` and `max_user_watches` don't exist. At least 300
  instances and 2,000 watches per process worked on the device (the probe's
  caps; no limit was hit).
- `readv` on an inotify fd goes straight to QNX's native `readv`, which
  returns whole records. A zero-length `read` or `readv` with an event
  queued returns `EAGAIN` where Linux returns `EINVAL`.

Other limits:

- ARM `svc #0` patching is supported. Thumb `svc` and TLS reads are patched
  only in statically loaded binaries that carry a symbol table, not in
  libraries mapped at run time.
- Priority-inheritance and robust-list futex operations are unsupported.
  Process-shared futexes do not synchronize separate Linuxemu host processes.
- Socket ancillary data other than `SCM_RIGHTS`, unlisted options, arbitrary
  ioctls, graphics, kernel modules, and Linux service management are
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
