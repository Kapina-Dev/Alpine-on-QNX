# Phase 3, part 1: full eventfd and epoll — design

Date: 2026-11-19. Roadmap: `docs/capability-roadmap.md`, "Phase 3: Linux
event interfaces". Approach chosen by the user: **A, eventfd backed by an
unnamed shared-memory counter plus a readiness pipe; epoll on QNX `poll()`.**

## Intent

Phase 3 adds the Linux event interfaces as objects in the Phase 1 virtual
descriptor layer. It splits into three parts, built and designed in order:

1. **eventfd and epoll** (this document);
2. timerfd and signalfd;
3. inotify, which starts with a device probe of QNX `ionotify()`.

The user follows the roadmap phases in order and chose **one release when
all of Phase 3 is finished**, not a release per part. Part 1 still ends with
its own gate (below) so part 2 starts from a green tree.

Why part 1 matters: Node.js stops at start-up because `epoll_create1`
returns `ENOSYS`; nginx and Redis fail their "basic" corpus stage for the
same reason (`docs/corpus-results.md`). epoll is also the core of Android's
`Looper` (one epoll fd watching an eventfd wake plus pipes and sockets),
which the Android track needs later. The eventfd is the first emulator
object whose state is not a native descriptor, so it validates the layer's
design for emulator-managed objects (roadmap Phase 3, "Full eventfd").

## Goals

- `eventfd`/`eventfd2` with full Linux semantics: 64-bit counter, any
  initial value, `EFD_SEMAPHORE`, `EFD_NONBLOCK`, `EFD_CLOEXEC`, blocking
  and nonblocking reads and writes, overflow handling, readiness, `dup`,
  `fork`, `exec` and `SCM_RIGHTS` passing.
- `epoll_create`, `epoll_create1`, `epoll_ctl` (`ADD`, `MOD`, `DEL`),
  `epoll_wait`, `epoll_pwait`, `epoll_pwait2`; level-triggered readiness,
  `EPOLLET`, `EPOLLONESHOT`, `EPOLLRDHUP`, `EPOLLEXCLUSIVE` accepted (see
  below), stable guest user data, nested epoll, and an epoll fd inside
  `poll`/`select`.
- Watched descriptors may be duplicated, closed, reused and modified while
  another thread waits (roadmap requirement).
- Node.js gets past `epoll_create1`; its next blocker is recorded.
- nginx and Redis pass the corpus "basic" stage, or their new blocker is
  recorded with the exact failing call.
- The full device regression passes with 0 failures and the corpus score
  does not drop.

## Non-goals

- timerfd, signalfd, inotify (parts 2 and 3).
- RIM's `/dev/android/epoll` (`epolld`) backend, and recovering its devctl
  protocol from `libbionic.so` (`docs/rim-epolld-protocol.md`). The roadmap
  keeps the backend as optional research; the `poll()` backend is the
  default regardless. Part 1 does only the quick probe below.

## RIM `epolld` quick probe (first task, user's choice 2026-11-19)

The roadmap says to probe `epolld` before writing epoll. The user chose
the quick form: with the Android runtime running, a native probe
(`native-probes/probe_epolld.c`) records the owner, group and mode of
`/dev/android/epoll` and `/dev/binder`, then tries to open
`/dev/android/epoll` and register a pipe as the ordinary user. The result
(expected: `EACCES`, like `/dev/binder` in the 2026-11-17 spike) goes into
`docs/FINDINGS.md` with the exact call and errno. Root and the protocol
recovery are out of scope. The epoll work doesn't depend on the outcome.
- A release. That happens after part 3.
- Fixing the python3 `0x60067dc4` crash. It didn't reproduce after the
  device reboot on 2026-11-18 and is a watch item (recorded in memory and to
  be added to `docs/FINDINGS.md` with this phase's findings).

## Anti-goals

- Success stubs. A case Linuxemu can't honour returns a Linux error and is
  documented, per the roadmap's design principles.
- A `select()`-only backend: its `FD_SETSIZE` would become a permanent
  epoll limit (roadmap).
- Regressing the existing wake eventfd that libcurl (and so `apk` and
  `git`) relies on.
- A helper thread or process per eventfd or per epoll instance.

## Constraints (measured or from the code)

- **The descriptor layer needs a native backing fd per object.** Objects
  are identified by the backing fd's `(st_dev, st_ino)` and polled through
  `guest_fd_poll_target()` (`src/guest_fd.c`). Every new object type
  therefore has a native fd whose readability tracks the Linux state.
- **Device facts (Passport, 2026-11-19, `build/shm-probe` on the device):**
  as the ordinary user, `shm_open` + immediate `shm_unlink` + `ftruncate` +
  `mmap(MAP_SHARED)` work and leave no name in `/dev/shmem`; a
  `PTHREAD_PROCESS_SHARED` mutex and condition variable in that memory work
  across `fork`; the shm fd passes over `SCM_RIGHTS` and the receiver's
  mapping shares the same counter. These go into `docs/FINDINGS.md`.
- `guest_fd_received()` today adopts only an object this process already
  holds (matched by identity). A fresh process receiving an eventfd needs
  more (see "Passing").
- Exec carry-over records (`LINUXEMU_FD_RECORDS`) carry one hidden fd per
  object through `ops->serialize` (`"hidden:hexpath"`).
- All guest waits already go through Linuxemu (`src/linux_poll.c`), which
  has the transient signal-wake protocol (`guest_thread_signal_wait_*`)
  used by `ppoll`/`pselect6` for temporary signal masks.
- Only `read` and `write` dispatch to typed objects today
  (`src/linux_syscall.c`, `LINUX_NR_READ`/`WRITE`).
- The guest's descriptors live below the hidden base (936 on the Passport).

## Approaches considered

**eventfd backing (the real choice):**

- **A. Shared memory + readiness pipe (chosen).** The counter and a
  process-shared lock live in an unnamed shm object. A pipe carries one byte
  whenever the counter is non-zero, so native `poll()` sees readability.
  No IPC on reads or writes. Cost: `EPOLLOUT`/`POLLOUT` can't come from the
  pipe and is synthesised (see below); passing to another process must carry
  the shm fd too.
- **B. Broker process.** One server owns all counters; the guest fd is a
  socket to it. Exact semantics, but every `eventfd_write` is an IPC round
  trip (libuv's `uv_async_send` and Android's `Looper::wake` call it
  constantly) and it adds a process lifecycle. Rejected for speed and
  complexity.
- **C. Process-local counter.** Diverges after `fork` and can't satisfy the
  roadmap's `SCM_RIGHTS` gate. Rejected.

**epoll backend:** one sensible way, from the roadmap: build a QNX `poll()`
set from the interest list on each wait. A helper thread per epoll that
keeps a readiness pipe current was rejected (a thread per instance, plus
wake latency); RIM's `epolld` is out of scope (non-goals).

## Design: eventfd

**State.** A new object type `GUEST_FD_EVENTFD` (already reserved in
`enum guest_fd_type`). Shared data in one page of unnamed shm:

```text
struct eventfd_shared {
    pthread_mutex_t lock;      /* PTHREAD_PROCESS_SHARED */
    pthread_cond_t  changed;   /* PTHREAD_PROCESS_SHARED; for blocking I/O */
    uint64_t counter;
    uint32_t pipe_full;        /* 1 while the readiness pipe holds its byte */
};
```

Per process, the object keeps: the readiness pipe's read end (the guest's
fd number; its identity is the object's identity), the hidden write end,
the hidden shm fd, and the mapping.

**Invariant.** Under `lock`: `pipe_full == 1` exactly when `counter > 0`,
and then the pipe holds exactly one byte. Every read and write updates the
counter and the pipe together under the lock (write one byte when the
counter goes 0 → non-zero; drain it when it returns to 0).

**Read** (`read`, `readv` with total length ≥ 8): length < 8 → `EINVAL`.
Counter 0: `EAGAIN` if the description is nonblocking, else wait on
`changed` (interruptibly, see below). Normal mode returns the counter and
sets it to 0; semaphore mode returns 1 and decrements.

**Write**: length < 8 → `EINVAL`; value `0xffffffffffffffff` → `EINVAL`.
If `counter + value` would exceed `0xfffffffffffffffe`: `EAGAIN` when
nonblocking, else wait on `changed`. Value 0 is a valid no-op write.

**Blocking waits are interruptible.** A blocking eventfd read or write must
return `EINTR` (or restart, per `SA_RESTART`, as Linuxemu already does for
native calls) when a guest signal arrives. The wait uses a timed or
signal-aware wait so the existing signal-delivery path can interrupt it; the
builder picks the mechanism (for example `pthread_cond_timedwait` in short
slices checking pending guest signals, or waiting in `poll()` on the
readiness pipe plus the thread's signal-wait fd and re-checking under the
lock). Correctness requirement: no lost wake-ups and no busy spin.

**Readiness.** `poll_fd` returns the pipe read end, so `POLLIN` is native.
`POLLOUT` is synthesised: an eventfd is writable unless
`counter == 0xfffffffffffffffe`. `do_poll`, `do_select` and epoll report
`POLLOUT` for an eventfd from the shared counter instead of the native
pipe read end (which never reports writable). The near-full case doesn't
wake a sleeping waiter; that is documented as a limit (a counter that close
to 2^64 doesn't occur in real workloads).

**Status flags.** `O_NONBLOCK` lives in `eventfd_shared` (each `eventfd()`
call gets its own page, which is exactly one Linux open-file description),
so a `fcntl(F_SETFL)` or `ioctl(FIONBIO)` in one process is seen by every
process sharing the eventfd, as on Linux.

**Hidden-fd budget.** Each process has 64 hidden fds (`HIDDEN_RESERVE`).
With a pipe write end and an shm fd per eventfd plus a wake write end per
epoll, about 20 Looper-style eventfd+epoll pairs would exhaust it. The
design keeps hidden fds per object to a minimum:

- First probe on the device whether a FIFO opened `O_RDWR` (made with
  `mkfifo` in a private directory and unlinked at once) gives one fd that
  is both pollable for reading and writable. If it does, the guest's fd is
  that FIFO and the readiness byte is written through it: an eventfd then
  needs only the hidden shm fd, and an epoll's wake channel needs none.
- If it doesn't, fall back to the pipe pair as described and raise
  `HIDDEN_RESERVE` so that at least 64 eventfd+epoll pairs fit, accepting
  the lower guest `RLIMIT_NOFILE`.
- Either way, running out returns `EMFILE`, as Linux does at the limit, and
  the per-process capacity is documented.

**A process dying while holding the lock.** A process killed inside the
critical section must not hang every other holder of the eventfd. The
critical sections never block (blocking waits happen in `poll()` outside
the lock), so the window is a few instructions, but `SIGKILL` and crashes
can still land there. The builder probes robust mutexes
(`pthread_mutexattr_setrobust`, `EOWNERDEAD`) on the Passport. If QNX
supports them, recover the state and mark the mutex consistent (the
invariant can be re-established from `counter` by refilling or draining
the pipe). If not, replace the mutex with a lock-free counter updated by a
64-bit compare-and-swap (`ldrexd`/`strexd`), with the pipe byte treated as
a hint that readers re-check.

**Lifetime.** The last reference in a process unmaps and closes the shm fd
and both pipe ends. The kernel frees the shm when no process holds it.

**fork.** Inherited as is: the shm mapping is `MAP_SHARED`, and the pipe
and shm fds are inherited. The child's object is the same Linux object.

**exec.** The serialize record carries the hidden fds the new image needs
(pipe write end and shm fd). The record format holds one hidden fd today;
the builder extends it to carry both (for example `"write:shm:"` in the
body) without breaking the wake eventfd's restore.

**Passing over `SCM_RIGHTS`.** The sending Linuxemu appends the object's
hidden fds (pipe write end, shm fd) to the `SCM_RIGHTS` array after the
guest's fds and marks the message so the receiving Linuxemu can tell. The
receiver strips the extra fds, rebuilds the object from them, and attaches
it to the guest's received fd. The guest's own fd count and order are
unchanged. The marker format is the builder's call; it must be robust to a
non-Linuxemu peer (a native QNX process receiving such a message gets the
extra fds; documented limit) and to a guest sending already-received fds
onward.

**Wake eventfd.** The current pipe-only `GUEST_FD_WAKE_EVENTFD` (libcurl's
`EFD_NONBLOCK|EFD_CLOEXEC`, initial 0, writes of 1) is replaced by the full
eventfd, so there is one implementation. Exec records of the old type keep
restoring (an exec from an old core into a new one is not supported, so
this only matters inside one build; the builder may drop the old type if
nothing still produces it).

## Design: epoll

**Object.** `GUEST_FD_EPOLL`. Process-local state:

```text
struct epoll_instance {
    pthread_mutex_t lock;
    struct epoll_item *items;   /* the interest list */
    int wake_read, wake_write;  /* private pipe: ctl/close wakes waiters */
    unsigned generation;        /* bumped by every ctl */
};
struct epoll_item {
    int fd;                     /* guest fd number as registered */
    dev_t dev; ino_t ino;       /* identity of the open file at ADD time */
    struct guest_fd_object *object;  /* referenced, if the fd is typed */
    uint32_t events;            /* Linux mask incl. EPOLLET/ONESHOT/... */
    uint64_t data;              /* guest user data, returned unchanged */
    int disabled;               /* ONESHOT fired, until EPOLL_CTL_MOD */
    int et_quiet;               /* EPOLLET: reported, waiting to re-arm */
};
```

The epoll fd's native backing is the wake pipe's read end (identity and a
real fd number for `dup`, `close`, `fork` and `exec`).

**`epoll_ctl`.** Validates as Linux does: `EBADF`, `EINVAL` (epoll fd is
not an epoll, `fd == epfd`, bad op), `EPERM` for a regular file or
directory (Linux refuses those), `EEXIST`, `ENOENT`, `ELOOP` for a nesting
cycle or depth over 5. `ADD`/`MOD`/`DEL` update the list under `lock`, bump
`generation` and write one byte to the wake pipe so a waiter in another
thread rebuilds its set.

**Registration key and close.** Linux keys an item by (fd number, open
file description) and drops it when the description is fully closed. Ours
keys by fd number and records the file's identity at `ADD`. On each wait,
an item whose fd is closed or now names a different file (identity
mismatch) is dropped silently, as Linux would have. A `dup` of a watched
fd keeps the item alive after the original number is closed on Linux; ours
loses it then. This is a documented limit (it needs the fd layer to track
open-file descriptions for native fds, which it doesn't).

**Closing a watched fd while another thread waits.** On Linux the waiter
isn't woken; the item just disappears. Here a blocked `poll()` still holds
the old native fd number, which may already name a new file. So every path
that closes or replaces a guest fd number (`close`, `dup2`/`dup3` onto it,
`close_range`, close-on-exec at `exec`) checks a per-fd "watched by an
epoll" bitmap and, if set, wakes the epoll instances watching it through
their wake channels. The woken waiter drops the stale item and rebuilds its
set without reporting anything for it. The builder probes on the device
what QNX `poll()` does when a descriptor in its set is closed by another
thread, and the tests pin the outcome: no event is ever reported for a
closed fd's old item, and a new file on the reused number is not watched
until it's added.

**`epoll_wait` family.** Under `lock`, build a `pollfd` array from the
enabled items (`poll_target` for typed fds, Linux events translated to
host), plus the wake pipe, plus the signal-wait fd for `epoll_pwait`/
`epoll_pwait2` with a mask (reusing the `ppoll` signal protocol from
`linux_poll.c`). Release the lock, `poll()`, then re-take the lock and:

- drain the wake pipe; if `generation` changed and nothing was ready,
  rebuild and wait again with the remaining timeout;
- translate `revents` to Linux, adding `EPOLLRDHUP` when requested and the
  host reports a peer shutdown (`POLLHUP` on a socket, or a zero-length
  `MSG_PEEK` read — builder's call);
- synthesise eventfd `EPOLLOUT` and nested-epoll readiness (below);
- apply `EPOLLONESHOT` (disable after reporting) and `EPOLLET` (below);
- copy at most `maxevents` events with the stored `data`; rotate the
  starting item between calls so one busy fd can't starve the others.

`epoll_pwait2` takes a `timespec` timeout (nanoseconds rounded up to
milliseconds, as `ppoll` does). `maxevents <= 0` → `EINVAL`.

**`EPOLLET` (approximation).** QNX `poll()` is level-triggered. After an
edge-triggered item is reported for an event, it is "quiet" for that event
until the guest performs an operation on that fd that could consume or
produce data: `read`, `readv`, `pread*`, `recv*`, `write`, `writev`,
`send*`, `accept*`, `sendfile`, or until the next `EPOLL_CTL_MOD`. Those
syscall paths clear the quiet flag through a cheap per-fd hook (a global
"fd has an ET watcher" bitmap keeps the hot path cheap). Difference from
Linux: new data arriving while the guest hasn't touched the fd produces no
new event until it does. Programs written for ET always drain until
`EAGAIN`, so this matches their behaviour; the difference is documented.

**`EPOLLEXCLUSIVE`.** Accepted on `ADD` (Linux validation rules) and
treated as a plain registration; all waiters may wake. Documented.

**Nested epoll and epoll in `poll`/`select`.** When an epoll fd appears in a
`poll`/`select` set or in another epoll's interest list, Linuxemu expands
it: its enabled items are added to the native `poll()` set, and the epoll
fd is reported readable (`EPOLLIN`) if any of them is ready, with the same
`ET`/`ONESHOT` state left untouched (a readiness probe must not consume
events, as on Linux). Depth is capped at 5 (Linux's limit) and cycles are
already refused by `epoll_ctl`. `select` with an epoll fd uses the same
expansion before calling `select()`, within `FD_SETSIZE`.

**fork.** Linux shares one epoll instance between parent and child. Ours
gives the child a copy of the interest list (items keep their data; typed
objects gain a reference). Changes in one process aren't seen by the other.
Documented limit: nginx, Redis, Node/libuv create their epoll after forking
or exec straight away.

**exec.** Linux keeps an epoll's interest list across `exec` when the fd
lacks `EPOLL_CLOEXEC`. The builder carries the list in the exec record when
it fits; otherwise the restored instance is empty and that is documented.
(Rare: libuv and nginx always use `EPOLL_CLOEXEC`.)

**`SCM_RIGHTS`.** An epoll fd received from another process starts native
(the receiver sees a pipe). Documented limit; no known workload passes
epoll fds.

## Tests

Guest tests in `guest-tests/` (C, built with the existing fixture tooling)
plus device stress, following the Phase 1/2 pattern:

- **eventfd**: counter semantics, semaphore mode, overflow (`EAGAIN`,
  blocking write released by a read in another thread), `EINVAL` cases,
  nonblocking reads, blocking read interrupted by a signal (with and
  without `SA_RESTART`), `poll`/`select`/epoll readiness both directions,
  `dup`, `fork` (child writes, parent reads), `exec` carry-over, and the
  roadmap Phase 1 extra gate: an eventfd and a native pipe sent over
  `SCM_RIGHTS` to a forked child that has closed its inherited copies, both
  still working there.
- **epoll**: every `epoll_ctl` error; level-triggered, `EPOLLET`,
  `EPOLLONESHOT`, `EPOLLRDHUP`; user data round trip; `maxevents` limits and
  fairness; `epoll_pwait` with a mask; `epoll_pwait2` timeout; nested epoll
  and an epoll fd in `poll`; a watched fd closed, its number reused, and the
  item dropped; the roadmap's concurrent cases (fd added, modified, removed
  and closed while another thread waits).
- **Android Looper pattern** (roadmap Android addition): one epoll watching
  an eventfd wake plus pipes and sockets, level-triggered, `epoll_wait` with
  timeouts, wakes from another thread, fds added and removed while waiting.
- The libcurl wake path keeps working: `apk` and `git` smokes in the full
  regression.

## Gate for part 1

- All new guest tests pass on the Passport.
- Full device regression: 0 failures.
- Corpus: Node.js past `epoll_create1` with its next blocker recorded;
  nginx and Redis "basic" stage pass or a new blocker recorded; the
  compatible count doesn't drop.
- `docs/compatibility.md`, `docs/FINDINGS.md`, `docs/corpus-results.md` and
  the roadmap's Phase 3 status updated, including every documented limit
  above.

## Decided by the user

- eventfd backing: approach A (shared memory + readiness pipe).
- Phase 3 is three parts in roadmap order; one release after part 3.
- The python3 crash is a watch item, not a blocker.
- RIM `epolld`: quick probe only (owner/mode, open and register as the
  ordinary user); no protocol recovery.

## My calls (correct me)

- Replace the wake eventfd with the full eventfd rather than keeping two.
- epoll registrations are keyed by fd number plus file identity; a watched
  `dup` that outlives the original number is a documented limit.
- `EPOLLET` is approximated by re-arming on guest I/O to the fd.
- `EPOLLEXCLUSIVE` is accepted and treated as a plain registration.
- Fork gives the child an independent copy of each epoll instance.
- Nesting depth cap of 5, matching Linux.
- An eventfd's `O_NONBLOCK` is shared across processes (stored in the shm).
- Keep hidden fds per object to a minimum (FIFO `O_RDWR` probe first; else
  a larger hidden reserve); running out returns `EMFILE`.
- A process dying inside the eventfd lock must not hang others: robust
  mutexes if QNX has them, else a lock-free 64-bit counter.
- Closing a watched fd wakes the epoll waiters so they drop the item.

## Left to the builder

- The interruptible blocking-wait mechanism for eventfd (requirement above).
- The exec record format for multi-fd objects and the `SCM_RIGHTS` marker.
- How `EPOLLRDHUP` is detected on QNX sockets (measure on the device).
- Data-structure choices (list vs array) and task ordering.
