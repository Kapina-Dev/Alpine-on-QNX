# Linuxemu capability roadmap

## Goal

Develop Linuxemu into a broad, dependable Alpine ARM32 compatibility layer for
BlackBerry 10/QNX. Common command-line tools, language runtimes, build tools,
databases, and lightweight servers should install and run with predictable
behavior.

Linuxemu is a WSL1-inspired user-space ABI translator, not a Linux kernel. The
project should advertise a tested compatibility surface rather than promise
arbitrary Linux binary compatibility.

### Second goal: Android 6 track

Use the same translator to run unmodified **WhatsApp** and **Viber** APKs on
BB10 (Passport first, then Classic) by booting a stock 32-bit ARM Android
6.0.1 (API 23) userland as Linux guest processes. QNX stays the kernel,
nothing is flashed, and a native BB10 launcher BAR opens the Android UI in a
QNX Screen window.

API 23 is the current floor: since 2026-09-08 WhatsApp has required Android
6.0+, and Viber's help page states the same. Expect that floor to rise within
1–3 years, so avoid design choices that would make a later move to API 24–27
expensive.

The Android track does not fork the roadmap. Phases 0–7 carry **Android
additions** that extend their gates. Phases 9–11 add the Android-only layers
and start once the Android gate for Phases 0–7 passes.

## Design principles

- Preserve the current release regression while extending the ABI surface.
- Keep Bash installed and use `/bin/bash` as the default interactive shell.
  BusyBox `sh` triggers the terminal-status query that Term49 renders as
  `?????`; future releases must not regress to that behavior or rely on escape
  filtering to hide it.
- Implement Linux semantics explicitly rather than matching syscall names.
- Return correct Linux errors for unsupported behavior; do not use success
  stubs to inflate compatibility results.
- Translate every structure, flag set, and error namespace that crosses the
  Linux/QNX boundary.
- Add functionality in response to reproducible workloads and retain a test
  for every compatibility fix.
- Keep ordinary-user execution as the supported environment.
- Never redistribute RIM binaries. You may study them, and at runtime you may
  call the copies already on the user's device.
- Nothing irreversible on the device: no flashing and no boot-partition writes.
- Develop on the Passport (3 GB, 4× Krait 400, Adreno 330) and keep the Classic
  (2 GB, 2 cores, 720×720) working.
- Record every device finding in `docs/FINDINGS.md`: date, device, what was
  tried, result, and evidence. When a planning hypothesis proves wrong, record
  that and update this roadmap.
- When a BB10 permission or ability gate blocks work, document the exact
  failing call and errno before trying workarounds.

## Phase 0: measurable compatibility

Preserve the v0.1.5 regression as the baseline and add structured syscall
diagnostics containing:

- unsupported syscall number and name;
- calling executable;
- a safe argument summary;
- occurrence count; and
- returned Linux error.

Create a pinned application corpus covering BusyBox, apk, Python, Git, SQLite,
nginx, Node.js/libuv, Redis, Vim, GCC/make, and selected file-watching tools.
Score each application separately for:

1. package installation;
2. executable startup;
3. basic workload completion;
4. concurrent workload completion; and
5. clean shutdown and resource cleanup.

Installation alone must not count as application compatibility.

**Android additions.** Add an `android-probe` corpus entry. It runs selected
AOSP 6.0.1 armv7 binaries with diagnostics enabled: `toybox`/`toolbox`,
`logwrapper`, `servicemanager` startup, and bionic's `/system/bin/linker`
loading a trivial program. Give the probe its own rootfs with `/system/lib`
so that it doesn't disturb Alpine. Commit the unsupported-syscall report as
`docs/android-syscall-gap.md` and refresh it after every phase. It is the
measured to-do list for the Android additions below.

### Phase 0 terminal checkpoint (2026-10-14)

Attached tmux with `TERM=xterm-256color` remains unresolved. Increasing the
host send buffer did not fix it. The previous success report was incorrect:
the client and server stayed alive while identification stalled. The user
also confirms default cmatrix still freezes Term49. Default cmatrix and
Ctrl-C did work over SSH, but that does not establish physical Term49 support.

The permanent socket regression covers a 2 KiB nonblocking `SCM_RIGHTS`
frame, but does not reproduce the multi-message tmux failure. Captured traces
show successful descriptor-bearing sends absent from server receives.
See `root-analysis/terminal-debug-20261014/README.md` for the saved evidence,
native control probe, device state, and next steps. Phase 0 is incomplete.

The release regression on the current long-running device session passes the
first 12 suites and the final Git suite. Python startup currently faults at
guest PC `0x60067dc4` with a null address before its first test marker. An A/B
run reproduces the identical fault with the installed pre-Phase-0 core and the
candidate, and both runs restore the rootfs package set. Treat this as a
fresh-device acceptance blocker, not as evidence against the socket fix.

### Validated device finding: threaded server teardown

On 2026-10-01, the Python HTTP failure described in the v3 roadmap was
reproduced on the Passport using the current `v0.1.3` Linuxemu binary and the
development rootfs. The standard threaded `python3 -m http.server` served its
first request with HTTP 200 and then Linuxemu terminated with `SIGSEGV` in QNX
`MsgSendnc_r`.

Syscall tracing showed that the final guest operation was `munmap` of the
handler thread's own approximately 2 MiB stack mapping. The QNX fault reference
was inside that just-unmapped range. This identifies the immediate defect as a
guest-thread self-unmap/exit lifecycle problem, not a general socket teardown
failure.

A single-threaded `HTTPServer` control completed all of the following without a
crash:

- 100 sequential connections;
- 10 clients that disconnected early;
- a 1 MiB response; and
- clean SIGINT shutdown with exit status 130.

The post-v0.1.3 development build now defers a physical self-stack unmap until
the guest exits and Linuxemu has returned to the native QNX pthread stack. It
performs that unmap before clearing the child TID and waking waiters. A direct
ARM regression fails with status 139 on v0.1.3 and passes with the fix; the
permanent Python suite also completes 100 sequential `ThreadingHTTPServer`
connections. Keep separate socket-close, shutdown, duplication, fork, and
early-disconnect tests because this does not prove every socket lifecycle case.

## Phase 1: virtual file-descriptor layer

Introduce a guest descriptor table capable of representing both native QNX
descriptors and emulator-managed Linux objects:

```text
guest fd
   `-- open-file description
         |-- native QNX fd, if any
         |-- object type
         |-- status flags
         |-- reference count
         |-- readiness operation
         |-- read/write operations
         `-- close/fork/exec operations
```

Initial object types should include native files and sockets, pipes, eventfd,
epoll, timerfd, signalfd, and inotify.

The layer must correctly implement:

- `dup`, `dup2`, `dup3`, and `fcntl(F_DUPFD*)`;
- shared open-file descriptions and status flags;
- close-on-exec behavior;
- descriptor replacement and number reuse;
- concurrent close and polling;
- fork inheritance and exec cleanup; and
- lifecycle management for hidden QNX descriptors.

Acceptance gate: every existing suite passes through the new layer, and stress
tests find no stale references or descriptor leaks across repeated duplication,
replacement, fork, exec, and close operations.

**Android additions.** Design these object types into the layer now and
implement them in Phase 9:

- `ashmem` regions: an fd backed by a QNX shm object, with size, name, and
  prot mask.
- binder handles: an fd representing a connection to the binder backend, with
  a per-process mmap'd receive buffer.
- **Cross-process passing of emulator-managed objects.** Both binder and
  `SCM_RIGHTS` move fds between processes, so an eventfd or ashmem object needs
  a representation that a different Linuxemu process can adopt.

Extra gate: an eventfd and a native pipe are sent over `AF_UNIX` with
`SCM_RIGHTS` to a forked child, and both still work there. This builds on the
current tmux `SCM_RIGHTS` work in Phase 0.

## Phase 2: contained modern syscall additions

Implement the relatively independent calls first:

- complete `pipe2` and `dup3` flag handling;
- `preadv` and `pwritev`;
- `fallocate`;
- `accept4`;
- `sendmmsg` and `recvmmsg`;
- additional common `fcntl64` operations; and
- broader resource-limit and resource-usage queries.

Acceptance gate: direct ARM tests cover valid use, invalid flags, partial I/O,
interruption, descriptor replacement, and concurrency. All current Python,
Git, apk, network, signal, and thread tests remain green.

**Android additions:**

- `prctl`: `PR_SET_NAME`, `PR_GET_NAME`, `PR_SET_DUMPABLE`,
  `PR_SET_NO_NEW_PRIVS`, and `PR_SET_PDEATHSIG`. Use real semantics where
  possible and proper errors otherwise.
- `setpriority`/`getpriority`, `sched_setscheduler`/`sched_getscheduler`, and
  `sched_setaffinity` with correct errors.
- `capget`/`capset`, at minimum reporting a consistent capability set. Zygote
  and installd query them.
- `personality`, and `getrlimit`/`setrlimit` for `RLIMIT_NOFILE` and
  `RLIMIT_STACK`.

### Phase 2 status: complete (2026-11-18)

Merged into master at `f4b2057` and installed on the Passport (the v0.1.6
release core is kept as `libexec/linuxemu-core.v0.1.6`). Design and plan:
`docs/superpowers/specs/2026-11-18-phase2-syscall-additions-design.md` and
`docs/superpowers/plans/2026-11-18-phase2-syscall-additions.md`. Supported
calls and limits: `docs/compatibility.md`; device facts: `docs/FINDINGS.md`.

**Scope.** The list above and its Android additions, widened on 2026-11-18
with every call the application corpus showed apps hitting (except `epoll`
and `inotify`, which stay in Phase 3) and the Node.js `sigaction` fix.

**What landed.**

- Signals: `rt_sigaction` queries and stores actions for the Linux signals
  QNX lacks (16, 48–64); sending one returns `EINVAL`. ARM `cacheflush`.
- Descriptors and I/O: `pipe2`/`dup3`/`accept4` flag and race coverage;
  `preadv`, `pwritev`, `preadv2`, `pwritev2` (with `RWF_NOAPPEND`, which
  musl's `pwrite` uses), `sendfile64`, `fallocate`; `fcntl64` pipe size,
  OFD locks and 64-bit record locks.
- Metadata: `statx`, `statfs64` (84- and 88-byte forms), `fchmod`,
  `faccessat2`, the `chown` family, `mknod`/`mknodat`.
- Sockets: `sendmmsg`, `recvmmsg`.
- Process and resources: `ugetrlimit`/`setrlimit`/`prlimit64` on 64-bit host
  limits, `getrusage`, `sysinfo`, `setitimer`/`getitimer`, priority (emulated),
  CPU affinity, `sched_*`, `capget`/`capset`, `prctl` dumpable /
  no-new-privs / parent-death signal, `membarrier` (reports no commands),
  `mremap` for plain anonymous memory.

**Bugs found and fixed on the way.** `accept4` blocked on an invalid flag;
QNX `dup2` isn't atomic against a concurrent `close` (now serialized, with
the lock reset in fork children); `sendfile64` lost data from a pipe; a NULL
iovec crashed the emulator; the core's 32-bit `off_t`, `rlim_t` and `flock`
truncated 64-bit offsets, sizes and limits in many paths; guest files were
opened without QNX `O_LARGEFILE`, so they couldn't grow past 2 GiB; the patch
table took duplicate entries and filled after about 4,000 executable flips.

**Gate results.**

- Full device regression: every suite passes except `run-python-smoke`,
  which fails with a python3 SIGSEGV at guest pc `0x60067dc4`. The pre-phase
  core fails identically, so it predates Phase 2; it was intermittent before
  and failed 4 of 4 runs on 2026-11-18. **Open; investigate before the next
  release.** `run-git-smoke`, which the gate never reaches, passes on its own.
- Application corpus: 7 of 11 compatible (same stages as the 2026-11-17
  baseline). Unsupported calls per app fell from 2–7 to 0–3; none of the
  Phase 2 calls is still `ENOSYS`. Results: `docs/corpus-results.md`.
- Node.js gets past its `ResetSignalHandlers` abort and now stops in its
  tracing agent's event loop after `epoll_create1` returns `ENOSYS`, the
  expected hand-off to Phase 3.

**Carried forward.**

- Phase 3: Node's start-up needs `epoll`; nginx and Redis need it too.
- Phase 4: the blind `svc`/TPIDRURO scan of memory a JIT makes executable can
  rewrite data words (measured; see `FINDINGS.md`). `mremap` refuses memory
  that was ever patched until then.
- Phase 6: carry `ITIMER_REAL` and the parent-death signal across `execve`;
  parent-death delivery (QNX has no notification); per-thread nice.
- Unscheduled: the `calloc` in the `sendmsg`/`recvmsg` path inside the signal
  handler; QNX `pwrite` ignores `O_APPEND`; QNX refuses any finite
  `RLIMIT_FSIZE`; `ITIMER_VIRTUAL`/`ITIMER_PROF` are unsupported on QNX.

## Phase 3: Linux event interfaces

Phase 2 left Node.js stopping here: its start-up creates an `epoll` instance
before it prints anything, and `epoll_create1` is still `ENOSYS`.

Implement the event family as clients of the virtual descriptor layer instead
of unrelated wrappers.

### Full eventfd

Support 64-bit counter behavior, blocking and nonblocking reads and writes,
overflow handling, semaphore mode, readiness, duplication, fork, and exec.
Use eventfd as the first validation of emulator-managed descriptors.

### epoll

Use QNX `poll()` as the primary native-readiness backend where possible. Do
not make `select()` the only backend because its descriptor-set limit would
become a permanent epoll limit.

Support:

- `epoll_create` and `epoll_create1`;
- `EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`, and `EPOLL_CTL_DEL`;
- level-triggered readiness;
- `EPOLLET`, `EPOLLONESHOT`, and `EPOLLRDHUP`;
- stable guest user data;
- close, duplication, and descriptor-number reuse;
- correct nested-epoll handling; and
- `epoll_wait`, `epoll_pwait`, and `epoll_pwait2`.

Reuse the existing transient wake-descriptor protocol for temporary signal
masks. Tests must specifically cover watched descriptors being duplicated,
closed, reused, and modified concurrently.

### timerfd

Use a central timer manager rather than one asynchronous signal handler per
timer. Track the Linux clock, next deadline, repeat interval, accumulated
expiration count, cancellation state, and pollable wake state.

Cover periodic overruns, absolute deadlines, clock changes, nonblocking reads,
duplication, fork/exec behavior, and close races.

### signalfd

Integrate signalfd with the existing per-thread Linux signal queues. A signal
must be consumed either through normal guest delivery or through signalfd, not
both. Cover standard-signal coalescing, real-time ordering, sender information,
blocking reads, polling, multiple signalfds, and thread-directed delivery.

### inotify

Probe the real device behavior of QNX `ionotify()` before selecting the
backend. Convert supported native notifications into Linux `inotify_event`
records and track watch descriptors, masks, rename cookies, watched-object
deletion, one-shot watches, queue overflow, and descriptor duplication.

Use a bounded metadata-scanning fallback or document a limitation where QNX
cannot produce equivalent behavior.

Acceptance gate: libuv event-loop tests pass; nginx can serve, time out,
reload, and stop; Node.js can run timers, files, subprocesses, sockets, and
signals; watcher tests pass create, update, rename, delete, and overflow cases.

**Android additions:**

- **Probe RIM's `epolld` before writing epoll. This is research only.**
  1. With the 4.3 runtime running, `native-probes/probe_epolld.c` records the
     owner, group, and mode of `/dev/android/epoll` and `/dev/binder`.
  2. It then opens `/dev/android/epoll` and registers a pipe, as the ordinary
     user and as root if available.
  3. Recover the devctl protocol from `libbionic.so`'s `epoll_*` and
     `_epoll_notify_close`, and document it in `docs/rim-epolld-protocol.md`.

  The native `poll()` backend stays the default regardless. A RIM backend is
  optional (`LINUXEMU_EPOLL=rim`) and works only with the 4.3 runtime
  installed.
- Android's `Looper` pattern passes. One epoll fd watches an eventfd wake plus
  several pipes and sockets, level-triggered, and `epoll_wait` runs with
  timeouts. Wakes come from another thread, and fds are added or removed while
  another thread waits.
- timerfd covers `CLOCK_BOOTTIME`. For `CLOCK_REALTIME_ALARM` and
  `CLOCK_BOOTTIME_ALARM`, either return `EINVAL` or map them deliberately, and
  document the choice. AlarmManager probes these clocks.
- inotify covers `IN_CREATE`, `IN_DELETE`, `IN_MOVED_*`, and `IN_CLOSE_WRITE`
  on directories, which is what FileObserver and PackageManager use.

### Phase 3 part 1 status: complete (2026-11-19)

Full eventfd and epoll, on branch `phase3a-events` (base `396a9d5`).
Design and plan:
`docs/superpowers/specs/2026-11-19-phase3a-eventfd-epoll-design.md` and
`docs/superpowers/plans/2026-11-19-phase3a-eventfd-epoll.md`. Supported
calls and limits: `docs/compatibility.md`; device facts: `docs/FINDINGS.md`.
Part 2 (timerfd, signalfd) is complete and part 3 (inotify) is done
pending merge (both below); one release after part 3.

**What landed.**

- eventfd: `eventfd`/`eventfd2` as a typed descriptor object, a 64-bit
  counter in shared memory updated by a lock-free `ldrexd`/`strexd`
  compare-and-swap, with a pipe as the pollable readiness channel.
  Semaphore mode, overflow, blocking reads and writes with signal
  interruption and `SA_RESTART`, `POLLOUT`, and one shared object across
  `dup`, `fork`, `execve` and `SCM_RIGHTS` (the hidden fds travel with the
  message under an `LXRIGHT1` descriptor map). libcurl's wake descriptor is
  now this eventfd.
- epoll: `epoll_create`/`create1`/`ctl`/`wait`/`pwait`/`pwait2` over QNX
  `poll()`. Level-triggered, `EPOLLET` (approximated), `EPOLLONESHOT`,
  `EPOLLRDHUP` (zero-length `MSG_PEEK`), `EPOLLEXCLUSIVE`, `EPOLLWAKEUP`;
  fair rotation; `ctl` and close wake a waiter in another thread; stale
  items are found by a per-fd-number epoch; nested epoll to depth 5 and
  epoll fds in `poll`/`select`; an independent copy across `fork`, the
  interest list across `execve`.
- `poll`/`select`: eventfd and epoll readiness, `POLLHUP` at pipe EOF, and
  host fd sets of 4096 bits.
- The hidden descriptor reserve went from 64 to 256, so the guest
  `RLIMIT_NOFILE` is 744 on the Passport (was 936).
- The Android Looper pattern passes (`looper-pattern`).
- The RIM `epolld` probe (research only): `/dev/android/epoll` is root:nto
  mode 777 and opens `O_RDWR` as the ordinary user, unlike `/dev/binder`.
  Its devctl protocol was not recovered; the `poll()` backend stays the
  only one.

**Bugs found and fixed on the way.**

- The per-thread signal-wait pipe that `ppoll`/`pselect6` create landed on
  guest-visible fd numbers; it is now hidden. Hiding it put it above QNX's
  `FD_SETSIZE` of 256, and `pselect6` returned `EMFILE`; fd sets are now
  4096 bits.
- QNX gives every socket of a family the same (st_dev, st_ino), so the
  design's identity check couldn't see one socket replacing another on a
  reused number. A per-number epoch replaced it.
- `epoll_ctl` from another thread didn't wake a blocked waiter; `ctl` now bumps a generation and writes the wake channel.
- An epoll lock held by another thread at `fork` stayed locked in the
  child; the child now gets fresh locks and its own wake channel.
- A race in the nested-epoll probe counter left a process-wide busy spin
  (unbounded with timeout -1); the epoll-nesting spin detector (check 17)
  pins the fix.
- Emulator limits that failed silently (exec list too long, nesting table
  full) are now reported in `LINUXEMU_DIAGNOSTICS`.
- `sendmsg` with more than 253 fds returns `EINVAL`, as Linux's
  `SCM_MAX_FD`.
- Final review fixes:
  - An epoll waiter polled and drained the guest's own epoll fd number. If
    another thread closed it, a later wake write raised `SIGPIPE`, and a
    reused number had its data eaten by the drain. The wait now uses a
    hidden duplicate of the wake read end (epoll-concurrent check 34).
  - Each blocking eventfd op and masked `ppoll`/`pselect6`/`epoll_pwait`
    made and closed a two-fd pipe, so 127 blocked threads exhausted the
    hidden reserve (`EMFILE` from `read`). Each thread now keeps one
    signal-wait channel, a persistent hidden pipe, drained at each wait
    and closed only at thread exit (numbers withdrawn before the close)
    and in the fork child (eventfd-blocking check 18, 100 threads). The
    limit stays about 127 concurrently-waiting threads. A one-fd
    self-connected loopback UDP socket was used briefly and removed: its
    stress test (`scripts/run-iopkt-stress.sh`) crashed io-pkt on the
    Passport (`docs/FINDINGS.md` 2026-11-19), and Linuxemu must never send
    io-pkt a message from a signal handler.
  - Exec records: one oversized record dropped every record; now only that
    record is dropped, with an `exec-fd-record-dropped` diagnostic.
  - The diagnostics errno table names `E2BIG`.

**Gate results.**

- All nine `run-event-smoke` fixtures pass on the Passport.
- Full device regression: every suite passes, `linuxemu_full_regression_failures=0`
  (core `fd053762…`), including `run-python-smoke` and `run-git-smoke`. The
  python3 SIGSEGV at `0x60067dc4` didn't appear. The Python smoke's old
  check that `select.epoll()` fails with `ENOSYS` was turned into a
  positive epoll check.
- Application corpus: 7 of 11 compatible. Redis is newly compatible (all
  five stages on epoll); BusyBox's shutdown stage failed once and passed in
  three reruns (intermittent, watch item). No app reports an eventfd or
  epoll call any more. Results: `docs/corpus-results.md`.
- Node.js gets past `epoll_create1` and now dies at start-up with a SIGSEGV
  at address 0, after `madvise` (`ENOSYS`) and a refused `mremap`.
- nginx creates its epoll (`using the "epoll" event method`), but
  `ioctl(FIOASYNC)` on the master/worker socketpair returns `ENOTTY`, so it
  starts no worker and never answers.

**Carried forward.**

- nginx: `ioctl(FIOASYNC)` on sockets. nginx's next call on that fd is
  `fcntl(F_SETOWN)`, which Linuxemu answers with `EINVAL` (QNX returns
  `ENOSYS`), so it is likely the blocker after that. Unknown ioctls return
  `ENOTTY` without a `LINUXEMU_DIAGNOSTICS` entry and should be reported.
- Node.js: the start-up SIGSEGV (pc `0x0116d688`, address 0) after
  `madvise` `ENOSYS` and `mremap` `mremap-kind`; likely V8 memory or JIT
  work (Phase 4/6).
- The intermittent BusyBox corpus shutdown failure ("still-alive" after
  `wait` returned 143).
- A guest-copy helper that returns `EFAULT` for a bad guest buffer: an
  eventfd `read`/`write` into unmapped memory kills Linuxemu today (the
  existing `ioctl`/`writev`/pipe convention), and epoll's `EFAULT` check
  proves a page mapped, not writable.
- `readv`/`writev` don't refuse hidden descriptors (pre-existing; matters
  more with eventfd's hidden fds).
- A thread's signal-wait pipe is made once and briefly holds the lowest
  free guest fd numbers while it is moved into the hidden range. Threads
  that ever waited keep two hidden fds while idle (about 127
  concurrently-waiting threads at most).
- Isolate the io-pkt crash (`docs/FINDINGS.md` 2026-11-19): run the
  pieces of `guest-tests/iopkt-signal-stress.c` one at a time (the UDP
  channel written from handlers, AF_UNIX `SCM_RIGHTS` ping-pong, path
  bind/unlink under signals, TCP, thread churn), with the user present to
  reboot, since a crash leaves the phone without network until reboot. If
  plain guest socket use under signals triggers it, consider a
  qnxBlockop-style guard (keep signals off a thread while it is inside an
  io-pkt call).
- Every `epoll_ctl` takes the process-wide `fd_replace_lock`, a throughput
  cost for Looper/Binder-style `ctl` churn.
- M4 (review): `guest_fd_acquire`'s stale-record drop can release the
  last reference under an epoll instance lock; if that were an epoll
  object its release would take the registry lock against the lock order.
  No path reaches it today (it needs an epoll number closed natively
  without `guest_fd_clear`); a guard comment marks it in `guest_fd.c`.
- M5 (review): `fork` while another thread is mid-way through an epoll
  instance edit (`ctl`, close hook) can hand the child a half-updated
  interest list; the child re-initialises the locks but doesn't repair the
  list.
- Smaller items from the reviews: an eventfd `F_SETFL` keeps
  only `O_NONBLOCK`; a failed exec restore leaks two carried hidden fds;
  a current-generation waiter can loop briefly while a wake byte remains;
  `ppoll`/`pselect6` return `EINTR` when an fd is ready and an unblocked
  signal is pending (Linux returns the count).
- Every limit in `docs/compatibility.md` ("Limits of the Phase 3 part 1
  calls") stays as documented: fork-copied epoll, registrations keyed by fd
  number, the `EPOLLET` approximation, `EPOLLEXCLUSIVE` waking all waiters,
  late `EPOLLRDHUP`, pipe hangups with data left, the near-full eventfd
  writer, epoll fds over `SCM_RIGHTS`, and the capacities.

### Phase 3 part 2 status: complete (2026-11-20)

timerfd and signalfd, on branch `phase3b-timers-signals` (base `37443ed`).
Design and plan:
`docs/superpowers/specs/2026-11-19-phase3b-timerfd-signalfd-design.md`
(with a "Corrections after implementation" note) and the matching plan
under `docs/superpowers/plans/`. Supported calls and limits:
`docs/compatibility.md` ("Limits of the Phase 3 part 2 calls"); device
facts: `docs/FINDINGS.md`. Part 3 (inotify) is next; one release after
part 3.

**What landed.**

- timerfd: a per-process timer manager with one QNX timer per armed
  timerfd delivering pulses, expirations computed from the clock,
  `CLOCK_BOOTTIME` and the `*_ALARM` clocks mapped to the plain clocks,
  `TFD_TIMER_CANCEL_ON_SET`, and sharing across `fork`, `execve` and
  `SCM_RIGHTS`.
- signalfd: a collector thread feeding a process-wide queue, delivery that
  stays exact (a signal is consumed once, by a handler or by the signalfd),
  masked waits that see the queue, and `fork`, `execve` and `SCM_RIGHTS`
  carry with a readiness channel per process.
- Signal fixes found on the way: a guest `sigprocmask` now reaches the host
  mask (written into QNX's handler frame, with a start-up self-check),
  real-time signals at the default action get `SA_SIGINFO`, and pending
  signals survive `execve` through `LINUXEMU_GUEST_PENDING`.
- Gate: `run-event-smoke` fixtures `timerfd-basic`, `timerfd-share`,
  `signalfd-basic`, `signalfd-semantics`, `signalfd-share`; full regression
  `linuxemu_full_regression_failures=0`.

**Follow-ups (not blocking part 3).**

- `rt_sigpending` and `rt_sigtimedwait` still return `ENOSYS`; whoever
  adds them must include the process queue.
- Stop-default signals (`SIGTSTP`, `SIGTTIN`, `SIGTTOU` at `SIG_DFL`) end
  masked waits with a bare `EINTR`; Linux keeps sleeping or restarts.
- The collector's race-path wake-up pokes every thread with the signal
  unblocked and has no `SA_RESTART`: a rare spurious `EINTR`.
- No masked-wait checks cover collected signals whose action is ignore or
  a terminating default.
- A signal collected between the exec snapshot and the host `execve` is
  lost; sender details, `sigqueue` values and real-time counts are lost
  across `execve`.
- A signalfd mask change reaches a sleeping process only at its next read
  or poll.
- A signalfd costs 4 hidden descriptors per process (about 64 in the 256
  reserve, shared with eventfd, timerfd (2 each), epoll and the per-thread
  signal-wait pipes); a fork child that can't build its readiness channel
  can sleep in `poll` until the timeout. With the reserve full,
  `rt_sigsuspend` falls back to the host `sigsuspend`, which can miss a
  signal queued just as the wait starts.
- The readiness byte can outlive a signal consumed by a handler or
  `sigwaitinfo` (`POLLIN`, then `EAGAIN`).
- timerfd: a timer stops firing for others when its arming process exits;
  relative `CLOCK_REALTIME` timers shift on a clock step; when `settime`
  races a count update in another process, that update can, rarely, add
  the old arming's uncounted expirations (bounded by what it produced).
- Real `CLOCK_REALTIME` change detection and `monotonic_in_sleep` are
  unverified on the device (`clock_settime` is `EPERM`; sleep can't be
  forced unattended).
- `snprintf` in the `execve` dispatch path (an existing pattern, not
  signal-safe).
- The io-pkt crash isolation from part 1 is still open.

### Phase 3 part 3 status: done, pending merge (2026-11-20)

inotify, on branch `phase3c-inotify` (base `fce297c`). Design and plan:
`docs/superpowers/specs/2026-11-20-phase3c-inotify-design.md` and
`docs/superpowers/plans/2026-11-20-phase3c-inotify.md`. Supported calls and
limits: `docs/compatibility.md` ("Limits of the Phase 3 part 3 calls");
device facts: `docs/FINDINGS.md` (2026-11-20, "QNX native inotify"). One
release follows this part.

**What landed.**

- The probes found that QNX's libc has a native inotify whose records,
  mask bits and watch descriptors are Linux's, and that `ionotify()` and a
  stat-scanning fallback are no use (`EINVAL` on directories; 4.2 s per
  1,000-entry scan, whole-second mtimes). So the guest's inotify fd is the
  native one: `inotify_init`, `inotify_init1`, `inotify_add_watch` and
  `inotify_rm_watch` (`src/linux_inotify.c`) wrap it with a typed object
  that holds no state. No threads, no hidden descriptors, no polling.
- The object adds guest paths (working directory, guest symlinks,
  `IN_DONT_FOLLOW`), Linux's errors where QNX differs (`EAGAIN` for an
  empty nonblocking read, `EINVAL` for a zero mask and `IN_MASK_CREATE`,
  `EFAULT` for bad pointers), and `EOPNOTSUPP` with a diagnostic for
  filesystems QNX can't watch and for device nodes, where the native call
  can block forever on some ptys.
- Readiness in `poll`, `select` and epoll and blocking reads come from the
  native fd. `dup`, `fork` and `execve` share the native queue; after
  `execve`, and for a received fd, the object is re-attached by
  `/dev/fsnotify`'s identity. `sendmsg`/`sendmmsg` refuse an inotify fd in
  `SCM_RIGHTS` with `EOPNOTSUPP`, because QNX's receiver would fail
  `EACCES` and lose the message.

**Gate results.**

- `run-event-smoke` fixtures `inotify-basic`, `inotify-modes` and
  `inotify-share` pass on the Passport; each task ran its new checks
  first on a core without its code (the pre-phase core or the previous
  task's) and saw them fail.
- The Android FileObserver and PackageManager events (`IN_CREATE`,
  `IN_DELETE`, `IN_MOVED_*`, `IN_CLOSE_WRITE` on directories) are covered
  by `inotify-basic`.
- Application corpus: 9 of 11 compatible. inotify-tools is newly
  compatible (all five stages, no unsupported call), and every app
  compatible before (the seven baseline apps and Redis) still is; BusyBox
  passed its shutdown stage. Results: `docs/corpus-results.md`.
- Full device regression: `linuxemu_full_regression_failures=0` (core
  `13eac1ec…`), on the second run. The first run failed only
  `run-python-smoke`: python3 died at start-up with a SIGSEGV (pc
  `0x11210964`, address `0x79616dcc`) before its first check, after every
  earlier suite, including `run-event-smoke`, had passed. Run alone, the
  Python smoke then failed once in five fresh installs on this core (the
  part 2 signature, pc `0x60067dc4`, address 0) and passed five of five on
  the pre-phase core; the same workload run 15 times per core, alternating,
  on one install passed all 30 times. This is the start-up crash first seen
  in Phase 2 (also on that pre-phase core), and nothing ties it to
  inotify: Python's start-up doesn't use inotify, and the only part 3 code
  on its path is one `stat` of `/dev/fsnotify` at start-up and a type check
  in `readv` on typed fds. The cause is still unknown (watch item, below).

**Phase 3 as a whole stays open.** The roadmap's Phase 3 acceptance gate
is not met, so Phase 3 is not marked complete:

- libuv event-loop tests: not run in part 3, by the user's choice (the
  corpus target was inotify-tools only). Open.
- nginx serve, time out, reload and stop: blocked by `ioctl(FIOASYNC)` on
  the master/worker socketpair (`ENOTTY`) and, likely next,
  `fcntl(F_SETOWN)` (`EINVAL`). Not inotify work. Open.
- Node.js timers, files, subprocesses, sockets and signals: Node still
  crashes at start-up (SIGSEGV, address 0) after `madvise` (`ENOSYS`) and a
  refused `mremap`, likely Phase 4/6 V8 memory and JIT work. Open.
- Watcher tests for create, update, rename, delete and overflow: pass
  (`inotify-basic`, `inotify-modes`, and the inotify-tools corpus app).

**Follow-ups (not blocking the release).**

- Every limit in `docs/compatibility.md` ("Limits of the Phase 3 part 3
  calls") stays as documented: about 250 ms late events, the overflow
  record first, extra events from Passport background processes,
  `IN_MASK_CREATE` always `EINVAL`, no `SCM_RIGHTS`, device nodes and
  unwatchable filesystems `EOPNOTSUPP`, `IN_EXCL_UNLINK` untested, no
  `/proc/sys/fs/inotify`.
- The `ENOTTY` to `EOPNOTSUPP` mapping for unwatchable filesystems isn't
  reachable from a guest path today (the guest's `/dev` maps only device
  nodes), so it is untested.
- From the reviews: a zero-length read with an event queued returns
  `EAGAIN` (Linux `EINVAL`); `IN_MASK_CREATE`'s `EINVAL` has no
  diagnostic; `readv` with a bad `iov_base` isn't probed for `EFAULT`;
  the pointer check uses `msync`, so a `PROT_NONE` guard page passes it
  (as epoll's); with a bad fd and an inotify fd in one `SCM_RIGHTS`
  message, `sendmsg` returns `EOPNOTSUPP` where Linux gives `EBADF`;
  a partial `sendmmsg` batch is untested.
- The intermittent python3 start-up SIGSEGV in `run-python-smoke`, seen
  only on the first run after a fresh install (2 of 7 on this core, 0 of 5
  on the pre-phase core, 0 of 30 on a kept install). Not yet isolated: capture a
  core or a guest backtrace at the faulting pc next time it fires.
- From part 1 and 2, still open: the io-pkt crash isolation, nginx's
  `FIOASYNC`/`F_SETOWN`, Node's start-up crash, and part 2's follow-ups.

## Phase 4: Thumb and stripped-binary support

Implement safe Thumb guest decoding and interception:

- Thumb `svc` detection;
- Thumb guest TLS-read interception;
- mixed ARM/Thumb transitions;
- correct saved-PC advancement and CPSR Thumb state;
- runtime executable mapping and `dlopen` patching; and
- instruction-cache synchronization.

Phase 2 measured the cost of the current blind scan: when a JIT makes memory
executable, data words that look like `svc #0` or a TPIDRURO read are
rewritten into traps (`FINDINGS.md`, "Phase 2: runtime-mapping patch
hazard"). Fixing that belongs here.

Do not blindly scan arbitrary byte patterns. Restrict patching to validated
executable mappings and decoded instruction boundaries.

Reduce the section-header requirement in stages:

1. Keep section-based patching as the preferred path.
2. Add validated executable-segment decoding for stripped binaries.
3. Reject files where safe instruction boundaries cannot be established.

Acceptance gate: static, PIE, shared-library, and runtime executable mappings
work in ARM, Thumb, and mixed modes without modifying embedded data.

**Android additions:**

- **Runtime-mapping patching is mandatory.** Bionic's linker loads every
  library itself with `mmap`/`mprotect`, and ART maps `.oat` files with
  `PROT_EXEC`.
- Stage 2 (executable-segment decoding) is required. Most AOSP `/system/lib`
  and APK native libraries ship without section headers.
- Extra gate: stripped, mostly Thumb-2 AOSP 6.0.1 `libc.so`, `libm.so`, and
  `libutils.so` load through bionic's linker. A Thumb NDK test binary's raw
  `syscall()` calls are intercepted.
- Record a decision in `FINDINGS.md`: either Phase 9 also ships a **modified
  bionic** whose syscall stubs call the translator directly, or the track relies
  on patching alone.

## Phase 5: synthetic Linux procfs

Do not expose a mounted QNX procfs as Linux `/proc`; its semantics and layouts
are incompatible. Intercept virtual paths and serve their content through the
virtual descriptor layer.

Start with:

- `/proc/self/maps`;
- `/proc/self/exe`;
- `/proc/self/fd`;
- `/proc/self/cmdline`;
- `/proc/self/environ`;
- `/proc/self/status` and `/proc/self/stat`;
- `/proc/cpuinfo` and `/proc/meminfo`;
- `/proc/uptime`, `/proc/version`, and `/proc/loadavg`; and
- `/proc/sys/kernel/random/uuid`.

Generate `/proc/self/maps` from Linuxemu's guest mapping registry and ensure
`/proc/self/fd` agrees with its guest descriptor table. Add only the `/sys`
entries demanded by tested workloads.

Acceptance gate: Python self-inspection works, and selected `ps`, `top`,
`htop`, and system-information workloads either pass or fail only on documented
unsupported fields.

**Android additions:**

- `/proc/self/maps` must be exact, because ART, debuggerd, and libunwind parse
  it.
- Add `/proc/<pid>/stat`, `/proc/<pid>/cmdline`, and `/proc/<pid>/oom_score_adj`
  (writable, stored per process), plus `/proc/sys/kernel/pid_max` and
  `/proc/sys/vm/overcommit_memory`.
- For `/sys`, expect `/sys/devices/system/cpu/possible` and `online`. Add
  anything further only when a measured workload needs it.

## Phase 6: process and synchronization depth

Add features in response to measured workloads:

- robust futex lists and additional futex operations;
- process-shared futex support where correctness is achievable;
- `waitid` and additional process-style clone forms;
- process groups, sessions, and controlling-terminal behavior;
- safe guest credential bookkeeping;
- resource accounting and interval timers, including the Phase 2 carry-overs:
  `ITIMER_REAL` and the parent-death signal kept across `execve`; and
- remaining signal restart cases.

If cross-process futex behavior cannot be implemented correctly on QNX, return
a documented Linux error instead of reporting false success.

**Android additions.** These items are required for the Android track, not
just achieved where possible:

- **Process-shared futexes across fork** on `MAP_SHARED` memory. The property
  area and its waits depend on them. If QNX can't support them correctly, the
  Android track is blocked, so find out early.
- Robust futex lists (`set_robust_list`). Bionic registers them for pthread
  mutexes.
- `vfork` and `clone(CLONE_VM|CLONE_VFORK)`, correct enough for bionic's
  `posix_spawn` and `system()`.
- **Guest credentials.** Zygote calls `setresuid`/`setresgid`/`setgroups` to
  give each app its own UID, and installd `chown`s per-app data directories.
  Keep consistent per-process guest credentials. Back file ownership and modes
  with a documented metadata store, since every file is owned by a single QNX
  user. Permission checks that Android relies on must see the guest values.
- **Fork stress like zygote's.** A parent with about 30 threads, many mappings,
  and open fds forks 100 children in a row. Each child execs, or runs and exits
  cleanly, with no fd or mapping leaks.
- **Pid and tid virtualisation.** Bionic aborts when a pid or tid is above
  65535 (32-bit bionic mutexes hold 16-bit tids), and QNX pids are much larger
  (`FINDINGS.md`, 2026-11-17). Guests see small virtual pids and tids that stay
  coherent across the whole guest process set, including `kill`, `wait4`,
  `/proc`, `SCM_CREDENTIALS`/`SO_PEERCRED` and binder sender pids. Binder
  prerequisite B0 in `docs/binder-plan.md`.

## Phase 7: filesystem and networking depth

Filesystem priorities:

- extended attributes with a documented backing strategy;
- `statx`, `renameat2`, `copy_file_range`, and `sendfile`;
- sparse-file behavior and improved timestamp precision; and
- expanded file-locking compatibility tests.

Networking priorities:

- socket ancillary messages;
- guest UNIX-socket descriptor passing;
- additional IPv6 and multicast options;
- interface queries;
- batch message operations; and
- broader socket ioctl conversion.

**Android additions.** These are probe-first, because QNX support is
unverified:

- `AF_UNIX` `SOCK_SEQPACKET`, which logd and other daemons use.
- `SO_PEERCRED` returning *guest* credentials. `property_service` and logd use
  it to authorize clients.
- Abstract-namespace `AF_UNIX` addresses.
- `SO_PASSCRED` and `SCM_CREDENTIALS`.

### Android gate for Phases 0–7

- Every roadmap gate from Phase 0 to Phase 6 passes on the Passport, and so do
  the Android additions in Phases 0–7.
- `docs/android-syscall-gap.md` lists no unsupported syscalls for the
  `android-probe` corpus. Device-file ioctls are the exception, since Phase 9
  handles them.

## Phase 8: application-driven releases

| Release | Primary capability |
| --- | --- |
| v0.2 | Virtual descriptors, `pipe2`, `dup3`, and full eventfd |
| v0.3 | epoll and timerfd; nginx and basic libuv workloads |
| v0.4 | signalfd and inotify; Node.js workload suite |
| v0.5 | Thumb and stripped-binary compatibility |
| v0.6 | Synthetic `/proc` and process-inspection tools |
| v0.7 | Deeper synchronization, filesystem, and socket support |
| v1.0 | Stable documented application matrix and installer |

Android milestones are tagged separately and don't hold back v1.0:

| Tag | Android capability |
| --- | --- |
| android-m1 | Phase 9: image, ashmem, property area, binder; `servicemanager` + binder test |
| android-m2 | Phase 10: headless boot through `system_server`, `adbd`, `am start` |
| android-m3 | Phase 11: Screen window, input, launcher BAR; WhatsApp on Passport |
| android-m4 | WhatsApp and Viber on Passport and Classic, with performance notes |

## Phase 9: Android system image and device files

Starts once the Android gate for Phases 0–7 passes.

### System image

- Get a 32-bit ARM AOSP `android-6.0.1` userland: a generic armv7 build or
  your own AOSP build. Use an `eng` or `userdebug` variant so that SELinux can
  be disabled. Write the recipe in `android/BUILD.md`, with checksums.
- The guest rootfs has `/system`, `/data`, `/cache`, `/dev`, `/proc`, and
  `/sys`, kept separate from Alpine. Map a BB10 shared folder (for example
  `/accounts/1000/shared/downloads`) to guest `/sdcard`.
- SELinux is disabled or permissive, and `init.rc` is trimmed.
- AOSP `init` expects to be PID 1 as root and to `mount`, `mknod`, and load
  policy. Decide and document one of two approaches, with no success stubs:
  - a trimmed init that starts from a prepared rootfs; or
  - real guest bookkeeping for the calls init makes.

### Device files (virtual descriptor objects from Phase 1)

- `/dev/ashmem` supports `ASHMEM_SET_NAME`, `SET_SIZE`, `GET_SIZE`,
  `SET_PROT_MASK`, `GET_PROT_MASK`, `PIN`, `UNPIN`, `GET_PIN_STATUS`, and
  `PURGE_ALL_CACHES`. It's backed by QNX `shm_open` and must survive fork and fd
  passing.
- **The property area** is guest `/dev/__properties__`, a shared region mapped
  into every guest process. Property waits use the Phase 6 shared futexes.
  Writes go through init's `property_service` socket as normal.
- **`/dev/binder`** speaks 32-bit protocol version 7. The binder workstream is
  `docs/binder-plan.md` (decided 2026-11-18). In short, Linuxemu writes its own
  binder behind a backend interface:
  - **Backend 1 (build first): our own broker.** A Linuxemu-owned QNX process,
    like Wine's `wineserver`, holds binder state for every guest process:
    nodes, refs, transactions, threads and death notifications. Guest
    `/dev/binder` is a virtual fd object; Linuxemu intercepts the guest's
    `ioctl` in-process and forwards it over `ChannelCreate`/`MsgSend` or
    `AF_UNIX`. It must not use `resmgr_attach`, which ordinary processes
    can't call. It implements:
    - `BC_*`/`BR_*` command streams, transactions and replies, and one-way calls;
    - strong and weak reference counts, and death notifications;
    - passing fds and binder objects;
    - a per-process receive buffer backed by shm; and
    - `BINDER_SET_CONTEXT_MGR`, `BINDER_SET_MAX_THREADS`, and the looper thread
      protocol.
  - **Backend 2 (optional, later): RIM's `/dev/binder`,** only if
    player-context access is ever solved. RIM's driver isn't reachable as
    devuser (the driver checks credentials itself), its context manager is
    taken by RIM's 4.3 `servicemanager`, it reports uid 1000 for every
    process, and it only exists where RIM's runtime is installed. The
    guest-facing side (ioctl translation, pid virtualisation, fd passing) is
    shared by both backends. External results for this driver are summarized
    in `FINDINGS.md` (2026-11-16) and stay external until reproduced.
  - The same broker is the natural home for ashmem and the property area.

  Work items B1–B5 (design document, read-only probes, disassembly study of
  RIM's `binder` as a reference, the broker in a fixed feature order, and
  milestones with stock AOSP 6.0.1 binaries) are in `docs/binder-plan.md`.
  Test against AOSP's `binderLibTest` and `binderDriverInterfaceTest` where
  they build for 6.0.1. Otherwise write equivalents.
- AlarmManager uses `/dev/alarm` if it's present and timerfd otherwise.
  Provide the timerfd path. Also provide `/dev/null`, `/dev/zero`,
  `/dev/urandom`, `/dev/random`, `/dev/tty`, and `/dev/ptmx` + `/dev/pts`.

Gate (android-m1): `servicemanager` runs, and a native binder client and server
exchange transactions, fds, and death notifications.

## Phase 10: headless Android boot

Stub the audio, sensors, radio/RIL, camera, lights, power, and GPS HALs. Reach
these milestones in order:

1. `init` parses `init.rc`, starts services, and `property_service` works.
2. `logd` runs, and `logcat` shows output.
3. Zygote (`app_process -Xzygote /system/bin --zygote --start-system-server`)
   preloads classes and forks.
4. `system_server` stays up for more than 10 minutes with no crash loop.
5. `adbd` runs, and `adb shell pm list packages` works from a PC.
6. `dex2oat` compiles a test APK during `adb install`, and the app's activity
   starts. Verify this headlessly with `am start` and logcat.

Gate (android-m2): all milestones pass on the Passport, with logs in
`FINDINGS.md`. Record boot time and memory use.

## Phase 11: display, input, launcher, and apps

### Display

Start with software rendering: a gralloc and framebuffer/hwcomposer HAL that
copies SurfaceFlinger's final frame into one QNX Screen window, using
Android's software GLES (`libagl`).

- Render at 720×720 and let Screen scale it to 1440×1440 on the Passport.
- The Screen side belongs in the host/bridge, not the guest. Use RIM's
  `libframebuffer_screen.so`, `libhwcwindow.so`, and `libgralloc_screen.so`
  imports to see which `screen_*` calls work.

### Input

Turn QNX Screen touch and keyboard events into synthetic evdev devices
(`/dev/input/event*`, with `EVIOCG*` ioctls) for InputReader. Mapping the
Passport keyboard's touch gestures is optional.

### Launcher BAR

A native BB10 app that:

- stops RIM's 4.3 runtime with `stop-android.sh` when needed, which is
  required on the Classic;
- boots Android under Linuxemu and shows the Screen window; and
- restarts the 4.3 runtime when it exits.

### Apps

- **WhatsApp:** confirm the current APK still contains `lib/armeabi-v7a`. Then
  `adb install` it and register with the SMS or voice code typed in by hand.
  Verify sending and receiving messages, and in-container notifications.
- **Viber:** same checks. It may need microG for FCM push.

Gate (android-m3/m4): WhatsApp sends and receives on the Passport and then the
Classic, with performance notes. Viber follows.

Out of scope until android-m4 passes:

- hardware GLES through QNX EGL (risky because of the bionic/QNX libc ABI and
  TLS clash);
- one Screen window per Android window, modeled on `libhwcwindow`;
- per-app launcher BARs;
- notifications forwarded to the BlackBerry Hub through PPS; and
- audio and camera for calls.

Optional, and only if the user asks: a whatsmeow terminal client under Alpine
once epoll lands. It breaks WhatsApp's ToS, so the number risks a ban, and it
needs a primary phone that comes online periodically.

## Optional: a native Linuxemu terminal app

Added 2026-11-17 at the user's request. It isn't scheduled yet: start it only
once the Linux side is stable enough that the terminal, not Linuxemu, is what
limits users. It is not a gate for any release.

**Why.** Term49 limits what Linuxemu can show:

- BusyBox `sh`'s terminal-status query renders as `?????`;
- default `cmatrix` freezes Term49 but works over SSH; and
- attached tmux with `TERM=xterm-256color` stalls (see the Phase 0 terminal
  checkpoint).

Each time, it's hard to tell whether Linuxemu or the terminal is at fault. A
terminal built for Linuxemu removes that doubt.

**Scope:**

- full xterm-256color emulation through a proven emulator library (for
  example libvterm), not a hand-written parser;
- UTF-8;
- launching Linuxemu and the Alpine shell directly, with no wrapper scripts or
  `PATH` setup;
- mapping the Passport and Classic keyboards: Ctrl, Esc, Alt, arrow keys and
  function keys; and
- one QNX Screen window, sized for both the Passport (1440×1440) and the
  Classic (720×720).

**Shares work with Phase 11.** The Phase 11 launcher BAR needs the same native
BB10 app packaging, signing and install path. Solve that once. The terminal app
is the smaller first user of it.

**Before writing it:**

- Check whether Term48's or Term49's source is available under a usable
  licence, because forking may be cheaper than starting fresh.
- Never redistribute RIM binaries.

**Until then:** test terminal behaviour against a known-good reference (SSH
from a real xterm) as well as Term49, and record which one each terminal
finding comes from. The app then becomes mostly UI work on top of an
already-correct PTY and termios layer.

**Gate:**

- tmux (attached, `TERM=xterm-256color`), vim, `cmatrix` and `htop` all work
  inside the app on the Passport and the Classic, with no `?????` output; and
- the same programs behave identically over SSH.

## Reference: RIM's Android 4.3 runtime

Study specimen: https://github.com/stanw47/Blackberry-Classic-Research.
RIM *recompiled* Android 4.3 for QNX, with `libbionic.so` sitting on QNX
`libc.so.3`. Linuxemu instead runs stock Linux binaries and translates
underneath, so RIM's code shows how each Linux behavior maps onto QNX. It is
not something to reuse.

- `notes/session35-binder-over-qnx-deciphered.md`: binder is a userland resmgr
  at `/dev/binder`. `ioctl_binder` forwards `BINDER_WRITE_READ` (0xc0186201)
  via QNX `ioctl` + `MsgSendv_r`, with shm payloads. That note calls
  0xc108620c `BINDER_VERSION`; later external research identifies it as CFG
  (nr 12, a 0x108-byte struct) and VERSION as 0xc0046209. Only WRITE_READ is a
  confirmed literal; `docs/binder-plan.md` lists the command words to confirm
  by disassembly.
- `notes/session36-rim-port-architecture-map.md` maps AOSP subsystems to QNX:
  properties → PPS, logging → slog2, futex → `SyncCondvar*`,
  epoll → `poll`/`ionotify`, graphics → libscreen, audio → ALSA,
  camera → camapi.
- `binder/notes-session79-resmgr-ability-gate.md`: **`resmgr_attach` returns
  EPERM for non-system processes anywhere on BB10.** Be a client of existing
  resmgrs, or use brokers that don't call `resmgr_attach`.
- `specimens/epolld` is a resmgr at `/dev/android/epoll`, built on
  `iofunc_notify*` and `MsgDeliverEvent`.
- `notes/ANDROID-APK-TO-DRAWER-FLOW.md` documents BB10's own APK install flow,
  which ours bypasses.

The newer A11 binder, player-context and exec-trust work lives in the Classic
repo (`runtime/`, `binder/`, `ws1/`) and in
https://github.com/stanw47/Blackberry-Passport-Research (notes, sessions
48–52). It is summarized in `FINDINGS.md`. Neither repo has a license, so
reimplement from the documented protocol and don't copy their code.

Related: the BB10 root project (https://www.patreon.com/cw/bb10root) has been
updated. Its v4 update adds an integrity check to the Android runtime launch
script, which could interfere with a launch-script hook. Check it before working around a privilege or ability gate, such as
the `resmgr_attach` EPERM gate or the RIM `/dev/binder` and
`/dev/android/epoll` probes. The root policy below still applies: root may
help with diagnostics, one-time setup, or an optional broker, but it doesn't
replace ordinary-user execution.

Every release must pass:

- the existing full regression;
- Bash-default interactive-shell checks in Term49, including absence of the
  `?????` prompt artifact;
- descriptor and resource-leak stress;
- forced error and interruption tests;
- clean installation, upgrade, rollback, and removal;
- repeated application workloads; and
- rootfs integrity checks after injected failures.

## Root policy

Linuxemu must continue to run as the ordinary QNX user. Root may be used for
read-only diagnostics, restricted process inspection, or narrowly scoped
one-time setup, but it does not provide Linux syscall semantics.

In particular, root does not solve epoll, inotify, timerfd, signalfd, futex,
Thumb decoding, signal conversion, or Linux ABI layout differences. BB10 path
trust and ability policy can also reject newly built or BerryCore executables
in elevated contexts even when the effective UID is zero.

If future system-wide inspection needs privileged access, place it in a small,
optional broker with a strict request protocol. Do not run the main translator
as root or make package compatibility depend on root access.

## Implementation order

```text
Compatibility measurement
        |
[complete] Thread self-unmap/exit regression fix
        |
[complete] Virtual descriptor layer
        |
[complete] Phase 2 contained syscalls (pipe2 / dup3 and more)
        |
[complete] Full eventfd
        |
[complete] epoll
        |
[complete] timerfd / signalfd
        |
[done, pending merge] inotify
        |
Thumb and stripped binaries
        |
Synthetic /proc
        |
Process/sync depth (Android: shared futexes, credentials, zygote fork)
        |
        +--------------------------------------+
        |                                      |
Long-tail ABI and application work      Android image + ashmem/properties/binder
                                               |
                                        Headless Android boot
                                               |
                                        Display, input, launcher, WhatsApp/Viber
```

Each phase's Android additions are developed with that phase, not afterwards.

## Near-term success target

The next useful target is not an arbitrary percentage of the Alpine package
index. It is a reproducible workload matrix in which nginx, Node.js/libuv,
Redis, Python asyncio, Git, SQLite, Vim, and GCC all pass while every existing
Linuxemu regression remains green.

That target is ambitious but achievable within Linuxemu's user-space design.
