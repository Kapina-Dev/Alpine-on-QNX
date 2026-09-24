# Linuxemu capability roadmap

## Goal

Develop Linuxemu into a broad, dependable Alpine ARM32 compatibility layer for
BlackBerry 10/QNX. Common command-line tools, language runtimes, build tools,
databases, and lightweight servers should install and run with predictable
behavior.

Linuxemu is a WSL1-inspired user-space ABI translator, not a Linux kernel. The
project should advertise a tested compatibility surface rather than promise
arbitrary Linux binary compatibility.

## Design principles

- Preserve the current release regression while extending the ABI surface.
- Implement Linux semantics explicitly rather than matching syscall names.
- Return correct Linux errors for unsupported behavior; do not use success
  stubs to inflate compatibility results.
- Translate every structure, flag set, and error namespace that crosses the
  Linux/QNX boundary.
- Add functionality in response to reproducible workloads and retain a test
  for every compatibility fix.
- Keep ordinary-user execution as the supported environment.

## Phase 0: measurable compatibility

Preserve the v0.1.2 regression as the baseline and add structured syscall
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

## Phase 3: Linux event interfaces

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

## Phase 4: Thumb and stripped-binary support

Implement safe Thumb guest decoding and interception:

- Thumb `svc` detection;
- Thumb guest TLS-read interception;
- mixed ARM/Thumb transitions;
- correct saved-PC advancement and CPSR Thumb state;
- runtime executable mapping and `dlopen` patching; and
- instruction-cache synchronization.

Do not blindly scan arbitrary byte patterns. Restrict patching to validated
executable mappings and decoded instruction boundaries.

Reduce the section-header requirement in stages:

1. Keep section-based patching as the preferred path.
2. Add validated executable-segment decoding for stripped binaries.
3. Reject files where safe instruction boundaries cannot be established.

Acceptance gate: static, PIE, shared-library, and runtime executable mappings
work in ARM, Thumb, and mixed modes without modifying embedded data.

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

## Phase 6: process and synchronization depth

Add features in response to measured workloads:

- robust futex lists and additional futex operations;
- process-shared futex support where correctness is achievable;
- `waitid` and additional process-style clone forms;
- process groups, sessions, and controlling-terminal behavior;
- safe guest credential bookkeeping;
- resource accounting and interval timers; and
- remaining signal restart cases.

If cross-process futex behavior cannot be implemented correctly on QNX, return
a documented Linux error instead of reporting false success.

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

Every release must pass:

- the existing full regression;
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
Virtual descriptor layer
        |
pipe2 / dup3 / full eventfd
        |
epoll
        |
timerfd / signalfd
        |
inotify
        |
Thumb and stripped binaries
        |
Synthetic /proc
        |
Long-tail ABI and application work
```

## Near-term success target

The next useful target is not an arbitrary percentage of the Alpine package
index. It is a reproducible workload matrix in which nginx, Node.js/libuv,
Redis, Python asyncio, Git, SQLite, Vim, and GCC all pass while every existing
Linuxemu regression remains green.

That target is ambitious but achievable within Linuxemu's user-space design.
