# Phase 1: virtual file-descriptor layer — design

Date: 2026-11-17. Roadmap: `docs/capability-roadmap.md`, "Phase 1: virtual
file-descriptor layer". Approach chosen by the user: **B, hybrid**.

## Intent

Phase 3 adds Linux objects that QNX doesn't have: epoll, timerfd, signalfd
and inotify. Each one is a guest fd that Linuxemu implements itself. The
existing eventfd shows that today there is nowhere proper to keep such an
object. Its state lives in three fixed arrays indexed by the native fd
number, capped at 256. Those arrays are not thread-safe, are lost across
`exec`, and refuse `dup` on the eventfd. Phase 1 replaces them with one
per-fd record layer, so that later phases plug objects into it rather than
adding more arrays. It also fixes the descriptor bugs that layer exposes.

The user follows the roadmap phases in order. Phase 1's job is the layer and
its semantics, not new Linux objects.

## How descriptors work today

- **Guest fd = QNX fd, passed straight through.** QNX gives native fds their
  shared open-file descriptions, reference counts, status flags, offsets,
  close-on-exec and fork inheritance.
- **Per-fd state** sits in `src/linux_syscall.c` as fixed arrays of
  `MAX_TRACKED_FDS = 256` entries:
  - `fd_paths[]` holds the host path of an fd opened by path. It's used for
    `*at` directory fds, `fchdir` and `getdents`.
  - `directory_streams[]` holds the `DIR *` that `getdents` opens lazily
    from that path.
  - `eventfd_write_peers[]`: the "wake" eventfd is a pipe. The guest gets
    the read end, and the array holds the write end.
- **Known defects:**
  1. `dup`, `dup2`, `dup3`, `F_DUPFD` and `F_DUPFD_CLOEXEC` of a wake
     eventfd return EOPNOTSUPP.
  2. Fds 256 and above silently lose path tracking. `getdents`, `fchdir`
     and `*at` on them then fail with EBADF.
  3. `exec` (Linuxemu re-executes itself) keeps native fds but loses every
     record. An inherited directory fd or eventfd stops working.
  4. Fds received through `SCM_RIGHTS` (and through any path that creates
     an fd without clearing its slot) can inherit a stale path or `DIR *`.
  5. There's no locking. Concurrent `close`/`dup` from guest threads can
     double-free a path string.
  6. The eventfd's write end is an ordinary low-numbered fd the guest can
     see and close.
- `fork` is a real QNX `fork()`, so memory, and therefore any table, is
  copied into the child.
- `exec` passes state to the new image through `LINUXEMU_*` environment
  variables. `hidden_environment()` strips them from what the guest sees.
- `poll` and `select` hand guest fd numbers straight to QNX.

## Measured facts (Passport, 2026-11-17)

- **Kernel identity is usable on QNX.** Each pipe has a unique `st_ino`
  (`st_dev` 1). Both ends share it, it stays the same across `dup` and
  across `SCM_RIGHTS` delivery to a forked child, and a pipe created after
  a close gets a new inode. A socketpair's two ends also share one inode.
  So `(st_dev, st_ino)` can tell an eventfd's pipe apart from any other
  pipe.
- **The tmux fix landed first** (commit 4c75593). Every AF_UNIX stream
  socket now has a 256 KiB receive buffer, so fd-carrying messages are not
  silently dropped. All baselines below were measured with it: full
  regression 14/14, network smoke including `unix-rights-backlog`.

## Goals

1. **One record layer** (`src/guest_fd.c` / `.h`) that replaces
   `fd_paths`, `directory_streams` and `eventfd_write_peers`.
   - It's growable, has no 256 cap, and is protected by a lock.
   - Records are **reference-counted objects shared across `dup`'d
     numbers**, the way Linux shares an open-file description.
2. **Object types:**
   - designed in: native (no record), path-tracked, wake eventfd, and
     reserved enumerators for eventfd, epoll, timerfd, signalfd, inotify,
     ashmem and binder;
   - implemented now: the path-tracked records and the wake eventfd,
     migrated onto the layer.
3. **Correct semantics**, as the roadmap requires:
   - the `dup` family on every record type;
   - shared state across `dup`;
   - close-on-exec;
   - fd replacement and number reuse (`dup2`/`dup3` onto a live fd);
   - fork inheritance;
   - exec carry-over of records for fds that survive exec;
   - lifecycle of hidden QNX fds;
   - thread safety of the table.
4. **Every path that creates an fd number sets or clears its record:**
   - `open`, `openat` and `creat`;
   - `socket`, `socketpair`, `accept` and `accept4`;
   - `pipe` and `pipe2`;
   - `eventfd2`;
   - the `dup` family and `F_DUPFD*`;
   - fds received with `recvmsg` `SCM_RIGHTS`.

   **Every path that destroys one releases it:** `close`, and `dup2`/`dup3`
   replacing a target.
5. **Defense against stale records.** A typed (non-native) record stores
   the `(st_dev, st_ino)` of its backing native fd. When the record is
   used, a mismatch means the number was reused behind our back. The record
   is then dropped and the fd treated as native.
6. **Hidden emulator fds.**
   - Emulator-owned long-lived fds (today, the eventfd write end) are moved
     to a reserved high range with `F_DUPFD` and are close-on-exec.
   - The fd limit the guest sees (`prlimit64` / `getrlimit` for
     `RLIMIT_NOFILE`) is lowered to the bottom of that range.
   - **The guest's `RLIMIT_NOFILE` is virtualised** (*my call*).
     - At start-up Linuxemu raises the host soft limit to the hard limit
       and keeps it there.
     - The guest reads and sets a virtual soft/hard pair, capped at the
       bottom of the hidden range.
     - The hidden range stays fixed for the process's lifetime, and its
       base is carried across `exec` in `LINUXEMU_FD_RECORDS`, so hidden
       fd numbers stay valid.
     - A guest that *lowers* its limit isn't enforced on fd creation. That
       is a documented deviation, harmless in practice: programs rarely
       depend on getting EMFILE.
   - Guest `close`, `dup2`/`dup3` targets and `F_DUPFD` into the reserved
     range are refused with EBADF/EINVAL, as Linux would for numbers above
     the limit.
7. **The extra gate: `SCM_RIGHTS` adoption between fork relatives.**
   - A process that receives an fd checks its `(st_dev, st_ino)` against
     the live typed objects it knows. If it finds one, it attaches the
     received number to that object.
   - A forked child inherits its parent's table, so an eventfd sent from
     parent to child over `AF_UNIX` still works as an eventfd there.
   - A native pipe needs no record and works as it does today.
   - **Scope: narrow (the user's decision).** Adoption covers exactly the
     roadmap gate:
     - the object was created before the fork;
     - it is sent parent → child;
     - the receiver still holds its inherited reference.

     If any of these is not true, the received fd stays a plain native fd.
     This is a documented limitation.
   - **Recorded for later phases:** general cross-process adoption, covering
     objects created after the fork, child → parent, a receiver that has
     already closed its copy, and unrelated processes. Phase 2 (full
     eventfd, which redesigns eventfd's backing objects) and Phase 9's
     cross-process registry for binder and ashmem must provide it.

## Non-goals

- Full eventfd semantics: counter values, semaphore mode, blocking reads.
  That's Phase 2's "full eventfd". Phase 1 keeps the wake-eventfd subset
  and only moves it onto the layer.
- Implementing epoll, timerfd, signalfd or inotify (Phase 3). Their type
  enumerators and the ops interface exist; their behaviour does not.
- Decoupled guest/native fd numbering (option A). Records are keyed by
  guest number with identity numbering, so that a later move to A is a
  mapping change, not a rewrite.
- Linux's guarantee that an in-flight `poll`/`read` keeps a closed fd alive.
  Native fds keep QNX's semantics, which is how Linuxemu behaves today.
- Adopting received typed objects between unrelated processes. That needs a
  cross-process registry and belongs to the binder/ashmem work in Phase 9.
- Reworking `getdents` to share its directory offset with the guest's
  native fd. The `DIR *` moves onto the shared object (so `dup`'d numbers
  share one stream), but it is still opened from the path.

## Anti-goals (failure even if it works)

- Any regression in the 14-suite device regression or the corpus baseline
  (7/11 compatible, with the same failure stages).
- Extra syscalls on the read/write/poll hot path for native fds. A native fd
  must cost at most one lock-free check.
- Object state that only lives in process memory when it has to be shared
  across `fork` (for example an event counter). State shared between
  processes must live in a kernel object (a pipe, shm). Process memory may
  only cache it.
- Touching the user's uncommitted edits: the roadmap file, the trace-only
  diagnostic lines in `src/linux_socket.c`, `ANDROID6_PLAN.md`, and so on.

## Constraints

- **Toolchain:** the BB10 SDK gcc 4.6.3 (`-std=gnu99 -marm`), building on
  the Passport. Code must compile without warnings under
  `-Wall -Wextra -O2`.
- QNX facilities may differ from Linux. For example, `fdopendir` may be
  missing. Check on the device before relying on one.
- **Tests:**
  - Assembly guest tests (`guest-tests/*.S`, built by
    `scripts/build-native-probes.sh`) are for exact syscall semantics.
  - C guest fixtures are compiled inside the Alpine rootfs, as in
    `scripts/build-pthread-fixture.sh`, and committed under
    `guest-tests/fixtures/`. They are for stress tests.
  - Host-side unit tests follow `tests/diagnostics-test.c` with
    `LINUXEMU_TESTING`.
- **The user's uncommitted edits stay out of commits.** The two trace-only
  `runtime_trace_enabled()` blocks and `#include <stdio.h>` in
  `src/linux_socket.c` are the user's. Commits touching that file stage a
  copy without them (`git hash-object -w` plus
  `git update-index --cacheinfo`), compile-check that copy on the device,
  and leave the working tree untouched. *My call.*
- **Acceptance happens on the Passport:** the full regression with
  `ssh -tt` and the summary file, the corpus, and smoke.

## Approaches considered

| | A. Full table | **B. Hybrid (chosen)** |
|---|---|---|
| Mapping | Every guest fd → description → native fd/object; numbers decoupled | Numbers pass through; records only for fds that need state |
| Touches | Every fd-taking syscall, plus exec number remapping | Creation/destruction paths, the dup family, typed-object I/O, exec carry-over |
| Hidden fds | Truly hidden | Reserved high range plus lowered guest fd limit; not airtight |
| Close vs. in-flight poll | Linux semantics | QNX semantics (unchanged) |
| Regression risk | High (one big switch-over) | Low (incremental) |

The user chose B: it meets the roadmap's requirements with low regression
risk, and it can grow into A later. The two weaker areas (hidden fds, close
during an in-flight poll) are no worse than today and don't block Phase 3.

## Decided design

- **Module:** `src/guest_fd.c` / `src/guest_fd.h`. `linux_syscall.c`,
  `linux_socket.c` and `linux_poll.c` call its API. They no longer own
  per-fd arrays.
- **Table:**
  - a growable array of object pointers indexed by fd, with `NULL` meaning
    native;
  - one mutex protects the table and the reference counts;
  - the child re-initialises the mutex after `fork`, in the same place as
    `linux_futex_after_fork()`.
- **Object:**
  - type, reference count, and backing native fd identity `(dev, ino)`;
  - type-specific data: a path and lazy `DIR *` for path-tracked records,
    the hidden write-end fd for a wake eventfd;
  - an ops table:
    - read and write;
    - **`poll_fd`**: the native fd that QNX `poll`/`select` should watch for
      this object's readiness. For the wake eventfd it's its own number.
      Phase 3 objects return a hidden backing fd;
    - **get/set status flags** (`O_NONBLOCK`, `O_APPEND`) for
      `F_GETFL`/`F_SETFL`. The wake eventfd applies them to both pipe
      ends;
    - release on last reference;
    - exec serialise and exec restore.

    *My call:* readiness and status flags go in now, because Phase 3's
    epoll and timerfd need them, and adding them later would mean changing
    every object.
  - `linux_poll.c` translates through `poll_fd` only for typed fds (behind
    the lock-free check). It reports `POLLNVAL` (and EBADF for `select`)
    for numbers in the hidden range.
- **Hot path:** read and write consult the table only for typed objects. A
  lock-free "this fd is typed" check gates the locked lookup, so native fds
  pay no lock.
- **Exec carry-over:**
  - For each fd that survives exec and has a record, the record is
    serialised into one hidden environment variable,
    `LINUXEMU_FD_RECORDS`.
  - Paths are hex-encoded. Hidden write-end fds are listed with their
    numbers, and the hidden fds stay open across exec for the objects that
    need them.
  - The new image rebuilds the table at start-up, before the guest runs.
  - *My call:* an environment variable (the existing mechanism) rather than
    a temp file or a pipe.
- **Hidden range:** the bottom of the range is derived from the host soft
  `RLIMIT_NOFILE` minus a fixed reserve, measured on the device, rather
  than hard-coded. *My call.*
- **Order of work, each step keeping the suites green:**
  1. Table and object core, with host unit tests.
  2. Move path records onto the table (fixes defects 2 and 4, and part
     of 5).
  3. Move the wake eventfd onto it, with `dup` support and a hidden write
     end (fixes 1 and 6).
  4. Interception completeness: `SCM_RIGHTS` received fds and every
     creation path.
  5. Exec carry-over (fixes 3).
  6. Hidden range and guest fd limit.
  7. Stress tests (dup/replace/fork/exec/close and threads, checked for
     leaks and stale records).
  8. The extra gate: an eventfd and a pipe over `SCM_RIGHTS` to a forked
     child, both still working.

## Acceptance gates

1. Every existing suite passes through the new layer. That means the full
   device regression at its known state (13/14; the only failure is the
   pre-existing `run-python-smoke` fault at `pc=0x60067dc4`), smoke with
   0 failures, and the corpus at 7/11 with unchanged stages.
2. Stress tests find no stale records and no fd leaks after repeated `dup`,
   replacement, `fork`, `exec` and `close`, single-threaded and with
   threads. Leaks are counted as host fds open before and after.
3. The extra gate: an eventfd and a native pipe sent over `AF_UNIX`
   `SCM_RIGHTS` to a forked child both still work there.
4. A new guest test for each fixed defect fails before its fix and passes
   after it.

## Left to the builder

- Exact names, the layout of the ops table, and how the table grows.
- How the lock-free "typed" check is done (for example a bitmap or an
  atomic count of typed fds).
- The format of `LINUXEMU_FD_RECORDS`, as long as it is hidden from the
  guest environment and rejects malformed input safely.
- Whether each fixed defect gets its own assembly test or shares one with
  others.
