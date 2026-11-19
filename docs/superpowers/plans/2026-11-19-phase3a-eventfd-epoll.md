# Phase 3, part 1: eventfd and epoll — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full Linux `eventfd` and `epoll` as objects in the Phase 1
descriptor layer, so Node.js, nginx and Redis get past their epoll
blocker and Android's Looper pattern works.

**Architecture:**
- eventfd: a 64-bit counter in one page of unnamed shared memory, plus a
  readiness channel that holds one byte while the counter is non-zero, so
  native `poll()` sees `POLLIN`. `POLLOUT` is synthesised.
- epoll: a process-local interest list. Each wait turns it into one QNX
  `poll()` call, with a wake channel so `epoll_ctl` and closes in other
  threads interrupt a wait in progress.
- New code goes in two new files, `src/linux_eventfd.c` and
  `src/linux_epoll.c`. `src/linux_syscall.c` only dispatches; the build
  already compiles `src/*.c`.

**Tech Stack:**
- C (gnu99) with the BB10 SDK gcc 4.6.3, building on the Passport.
- C guest fixtures (musl, pthreads), built with the guest's gcc by
  `scripts/build-guest-fixture.sh`.
- Native QNX probes in `native-probes/`.

**Spec:** `docs/superpowers/specs/2026-11-19-phase3a-eventfd-epoll-design.md`.
It is the authority for every semantic. This plan doesn't repeat it, only
the decisions and the values tests pin.

## Global Constraints

- **Proper errors, never fake success.** Unsupported cases go through
  `linux_unsupported(number, error, "detail", args)` so they show in
  `LINUXEMU_DIAGNOSTICS`.
- **Every flag is translated explicitly.** Unknown Linux bits → `EINVAL`.
  No raw Linux constant reaches QNX.
- **Phase 1 descriptor rules hold.** A guest fd that `guest_fd_is_hidden()`
  reports → `EBADF`. New descriptors go through `guest_fd_admit()`; dups
  through `guest_fd_duplicate()`.
- **Signal safety.** Syscalls run inside the SIGILL handler. No `malloc`,
  `calloc`, `free` or stdio in new dispatch paths. Interest lists and poll
  arrays use `mmap`-backed storage (a small helper in `src/linux_epoll.c`)
  or fixed stack buffers.
- **Builds are warning-free** under `-Wall -Wextra -O2 -std=gnu99 -marm`.
- **Every test must fail on the pre-phase core and pass on the new one.**
  Before Task 2, copy the current core (`build/linuxemu`, sha256
  `247fdc99…`) to `build/linuxemu-pre-phase3` on the device. Show the
  failing run before each fix.
- **Tests:** C fixtures exit with a distinct code per check (10, 11, 12, …)
  and print the failing check and `errno` on stderr.
- **Phase order:** no timerfd, signalfd or inotify work. No `epolld`
  backend or protocol recovery.
- **Device:** the Passport, as `ssh -o HostName=<passport-ip> bb10`.
  - The IP changes; ask the user and never write it into the repo.
  - The dev tree is `/accounts/1000/shared/misc/linuxemu-dev` (no git).
    Sync from the worktree without `-tt`:
    `tar -cf - src scripts guest-tests native-probes tests | ssh … 'cd DEV && tar -xf -'`.
  - Build with `sh scripts/build-native-probes.sh`.
  - `run-full-regression.sh` needs `ssh -tt` with no pipes or redirects.
  - BerryCore `timeout` never fires; a hang needs a manual kill.
  - Never run `LINUXEMU_SYSTRACE` on a test that captures a child's stderr
    (it deadlocks on the pipe).
- **C fixtures:** build with
  `ALPINE_ROOT=build/fixture-rootfs sh scripts/build-guest-fixture.sh NAME`
  on the device and commit `guest-tests/fixtures/NAME-armhf`.
- **New suite:** `scripts/run-event-smoke.sh` (Task 2 creates it) runs every
  Phase 3 fixture through a `run_rootfs_fixture` copy (as in
  `scripts/run-process-smoke.sh:78`) and ends with
  `linuxemu_event_smoke_failures=N`. Task 2 adds it to
  `scripts/run-full-regression.sh` after `run-signal-smoke`.
- **Commits:**
  - Worktree branch `phase3a-events` (superpowers:using-git-worktrees).
  - One commit per task; the message records the device evidence (exit
    codes before and after).
  - No `Claude-Session:` trailer; end with
    `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Never stage the user's files:**
  `root-analysis/native-probe-results-20260922.md`,
  `native-probes/unix-rights-pressure.c`,
  `root-analysis/terminal-debug-20261014/`.
- **ARM EABI numbers and constants:**

  | Item | Value |
  | --- | --- |
  | eventfd, eventfd2 | 351, 356 |
  | epoll_create, epoll_ctl, epoll_wait | 250, 251, 252 |
  | epoll_pwait, epoll_create1, epoll_pwait2 | 346, 357, 441 |
  | EFD_SEMAPHORE, EFD_NONBLOCK, EFD_CLOEXEC | 0x1, 0x800, 0x80000 |
  | EPOLL_CLOEXEC | 0x80000 |
  | EPOLL_CTL_ADD, DEL, MOD | 1, 2, 3 |
  | EPOLLIN, PRI, OUT, ERR, HUP | 0x1, 0x2, 0x4, 0x8, 0x10 |
  | EPOLLRDNORM, RDBAND, WRNORM, WRBAND, MSG | 0x40, 0x80, 0x100, 0x200, 0x400 |
  | EPOLLRDHUP, EXCLUSIVE, WAKEUP, ONESHOT, ET | 0x2000, 1<<28, 1<<29, 1<<30, 1<<31 |
  | eventfd counter maximum | 0xfffffffffffffffe |
  | epoll nesting depth limit | 5 |

  `struct epoll_event` on ARM EABI is **16 bytes, not packed**: `uint32_t
  events` at offset 0, padding, `uint64_t data` at offset 8.

## Review Focus

Failure modes most likely to bite a real program that the per-feature tests
wouldn't otherwise hit. Each has a test in the named task.

1. **The libcurl wake path** (`EFD_NONBLOCK|EFD_CLOEXEC`, initial 0, writes
   of 1) must keep working after the wake eventfd is replaced: `apk` and
   `git` depend on it. Test: Task 2 keeps `wake-eventfd-dup` green and adds
   check 18 (that exact pattern).
2. **A large interest list:** nginx registers every connection. An epoll
   watching nearly the guest's whole fd limit (at least 700 fds) must wait
   and report correctly, with no `malloc` in the path. Test: Task 5,
   check 22.
3. **A watched fd closed and its number reused by `accept`** while another
   thread waits: no event for the old item, ever. Test: Task 6, check 31.
4. **A blocking eventfd read in a thread when the process gets a signal
   with `SA_RESTART`** must restart, not return a spurious `EINTR` or a
   short read. Test: Task 3, check 13.
5. **`epoll_wait` with a `NULL` or unmapped events pointer** must return
   `EFAULT`, not crash Linuxemu. Test: Task 5, check 23.

---

### Task 1: Device probes that decide the build

The quick `epolld` probe (user's choice) plus the four device facts the
spec leaves to measurement. Their results pick branches in Tasks 2, 3, 6
and 7.

**Files:**
- Create: `native-probes/probe_epolld.c`, `native-probes/phase3-probes.c`
- Modify: `scripts/build-native-probes.sh` (build both, `-lsocket`),
  `docs/FINDINGS.md` (one dated entry with each result, call and errno)

**Interfaces:**
- Produces findings named exactly as below; later tasks branch on them:
  `fifo_rdwr`, `robust_mutex`, `poll_close_race`, `rdhup_signal`.

- [ ] **Step 1: Write `probe_epolld.c`.** With the Android runtime running,
  print owner, group and mode of `/dev/android/epoll` and `/dev/binder`
  (`stat`), then `open("/dev/android/epoll", O_RDWR)` and, if it opens,
  try to register a pipe with the devctl call `libbionic.so`'s `epoll_ctl`
  uses (if that call can't be identified without protocol recovery,
  record "not attempted" — recovery is out of scope). Print each call's
  result and errno.
- [ ] **Step 2: Write `phase3-probes.c`**, one line per finding:
  - `fifo_rdwr=ok|fail:<errno>`: `mkfifo` in a private directory under
    `/tmp`, `open(O_RDWR|O_NONBLOCK)`, `unlink`; write one byte through the
    fd, `poll` reports `POLLIN`, read it back, `poll` (timeout 0) reports
    nothing; `fork` and the child's write wakes the parent's `poll`.
  - `robust_mutex=ok|absent|fail:<errno>`: a `PTHREAD_PROCESS_SHARED`
    robust mutex in shm; a child locks it and `_exit`s; the parent's lock
    returns `EOWNERDEAD`, then `pthread_mutex_consistent` and unlock work.
    `absent` when the attribute call doesn't exist or returns `ENOSYS`/
    `EINVAL`.
  - `poll_close_race=<what happened>`: thread A polls a pipe read end with
    a 2 s timeout; thread B closes that fd after 100 ms and opens a new pipe
    that reuses the number and writes into it. Record whether A returns
    early, what `revents` it sees (`POLLNVAL`? the new pipe's `POLLIN`?),
    or blocks until the timeout.
  - `rdhup_signal=<revents>`: loopback TCP pair; the peer calls
    `shutdown(SHUT_WR)`; record `poll` `revents` for `POLLIN|POLLHUP|POLLRDNORM`
    and whether `recv(MSG_PEEK)` returns 0. Repeat for an `AF_UNIX`
    stream pair.
- [ ] **Step 3: Build and run both on the device.**
  Run: `sh scripts/build-native-probes.sh && build/native-probes/phase3-probes && build/native-probes/probe_epolld`
  Expected: four `name=value` lines, and the epolld lines. Expect
  `EACCES` on the epolld open (as `/dev/binder` in the 2026-11-17 spike).
- [ ] **Step 4: Record in `docs/FINDINGS.md`** (date, device, what was
  tried, result, evidence), including the shm facts already measured on
  2026-11-19 (spec, "Constraints").
- [ ] **Step 5: Commit** ("Probe epolld and the Phase 3 device facts").

---

### Task 2: The eventfd object: counter, semaphore mode, nonblocking I/O

**Files:**
- Create: `src/linux_eventfd.c`, `guest-tests/eventfd-semantics.c`,
  `scripts/run-event-smoke.sh`
- Modify: `src/linux_syscall.c` (dispatch 351 and 356 to the new file;
  delete the `wake_eventfd_*` code at `src/linux_syscall.c:930-1075` and
  its restore registration), `src/linuxemu.h` (prototypes),
  `src/guest_fd.h` / `src/guest_fd.c` (new op, below),
  `scripts/run-full-regression.sh` (add `run_suite run-event-smoke` after
  `run-signal-smoke`)

**Interfaces:**
- Produces, in `src/linuxemu.h`:
  - `int32_t linux_eventfd_create(uint32_t initial, uint32_t linux_flags);`
    returns the guest fd or a negative Linux errno. `eventfd` (351) calls it
    with flags 0.
  - `void linux_eventfd_register(void);` registers exec restore (Task 4).
- Produces, in `struct guest_fd_ops` (append, so existing initialisers
  stay valid with a zero):
  `short (*ready_now)(struct guest_fd_object *, int fd);` returns host poll
  bits (`POLLOUT`, …) that are ready without native help. Used by
  `do_poll`, `do_select` (Task 3) and epoll (Task 5).
- Produces the shared layout (in `src/linux_eventfd.c`):
  `struct eventfd_shared { pthread_mutex_t lock; uint64_t counter;
  uint32_t channel_full; uint32_t nonblock; uint32_t semaphore; }` in one
  page. (Task 3 changes the lock if `robust_mutex` is `absent`.)

**Readiness channel (from Task 1):**
- `fifo_rdwr=ok`: the guest fd is the `O_RDWR` FIFO; no hidden write end.
  The shm fd is the only hidden fd.
- `fifo_rdwr` failed: pipe pair (read end = guest fd, write end hidden),
  and raise `HIDDEN_RESERVE` in `src/guest_fd.c` from 64 to 256 (three
  hidden fds per eventfd+epoll pair × 64 pairs, plus the existing users),
  which lowers the guest's `RLIMIT_NOFILE` from 936 to 744.

- [ ] **Step 1: Write `guest-tests/eventfd-semantics.c`** with these checks:
  - 10 setup failed.
  - 11 `eventfd2(5, 0)`; `read` returns 8 bytes holding 5; a nonblocking
    second read (`EFD_NONBLOCK` instance) fails `EAGAIN`.
  - 12 writes of 3 and 4 then a read returns 7 (counter sums, read resets).
  - 13 is reserved for Task 3.
  - 14 `EFD_SEMAPHORE` with initial 3: three reads each return 1, the
    fourth (nonblocking) fails `EAGAIN`.
  - 15 `read` with a 7-byte buffer and `write` with 7 bytes fail `EINVAL`;
    writing `0xffffffffffffffff` fails `EINVAL`.
  - 16 nonblocking: counter at `0xfffffffffffffffe - 1`, writing 2 fails
    `EAGAIN`, writing 1 succeeds.
  - 17 `poll` on a fresh eventfd (timeout 0) reports nothing; after a write
    it reports `POLLIN`.
  - 18 the libcurl pattern: `eventfd(0, EFD_NONBLOCK|EFD_CLOEXEC)`, write
    1 twice, one read returns 2, `fcntl(F_GETFD)` has `FD_CLOEXEC`.
  - 19 `eventfd2(0, 0x4)` (an unknown flag) fails `EINVAL`; `eventfd` (351)
    with initial 2 then `read` returns 2.
  - 20 `fcntl(F_SETFL, O_NONBLOCK)` on a blocking instance makes the next
    empty read fail `EAGAIN`; `F_GETFL` shows it.
  - 21 `dup` of an eventfd shares the counter and the status flags: a
    write through the dup is read through the original, and `O_NONBLOCK`
    set on one shows on the other.
  - 22 creating eventfds in a loop reaches at least 50 before failing, and
    the failure is `EMFILE` (not a crash or another errno); closing them
    all lets a new one be created.
- [ ] **Step 2: Create `scripts/run-event-smoke.sh`** running
  `eventfd-semantics` as a rootfs fixture, plus `wake-eventfd-dup` (the
  existing `.S` test, as `scripts/run-process-smoke.sh` runs `.S` tests).
  Add the suite to `run-full-regression.sh`.
- [ ] **Step 3: Build the fixture and run it on the pre-phase core.**
  Run: `LINUXEMU_ROOT=test-rootfs build/linuxemu-pre-phase3 test-rootfs/tmp/eventfd-semantics` (via the suite's copy helper)
  Expected: exit 11 (`EOPNOTSUPP` for the non-libcurl form).
- [ ] **Step 4: Implement `linux_eventfd_create` and the object ops** in
  `src/linux_eventfd.c`: `shm_open` + `shm_unlink` (name from pid and a
  counter) + `ftruncate(4096)` + `mmap(MAP_SHARED)`; shm fd hidden with
  `guest_fd_hide`; `PTHREAD_PROCESS_SHARED` mutex; read/write per the spec
  with the invariant "channel holds one byte exactly when counter > 0"
  maintained under the lock; `get_status`/`set_status` keep `O_NONBLOCK`
  in `nonblock` (the native fd itself stays nonblocking always, so a
  blocking path never blocks inside the lock); `ready_now` returns
  `POLLOUT` when `counter < 0xfffffffffffffffe`. Blocking (empty read, full
  write) returns `-EAGAIN` for now; Task 3 makes it block.
- [ ] **Step 5: Run the suite on the new core.**
  Run: `sh scripts/run-event-smoke.sh`
  Expected: `eventfd-semantics exit=0`, `wake-eventfd-dup` ok,
  `linuxemu_event_smoke_failures=0`. Also `sh scripts/run-package-smoke.sh`
  passes (libcurl through `apk`).
- [ ] **Step 6: Commit** ("Add the full eventfd object").

---

### Task 3: eventfd blocking waits, signals, `POLLOUT` and a dead lock holder

**Files:**
- Modify: `src/linux_eventfd.c`, `src/linux_poll.c` (`do_poll`,
  `do_select` consult `ready_now`), `guest-tests/eventfd-semantics.c`,
  `guest-tests/eventfd-blocking.c` (create), `scripts/run-event-smoke.sh`

**Interfaces:**
- Consumes: `ready_now` (Task 2); the thread signal-wait protocol
  `guest_thread_signal_wait_begin/arm/end` (`src/linux_poll.c` uses it).
- Produces: `int linux_eventfd_writable(struct guest_fd_object *)` — not
  needed outside; epoll uses `ready_now`.

- [ ] **Step 1: Write `guest-tests/eventfd-blocking.c`**:
  - 10 setup failed.
  - 11 a blocking read in thread B returns 9 after thread A writes 9
    100 ms later.
  - 12 a blocking write of 2 at `max - 1` in thread B completes after
    thread A reads; the counter is then 2.
  - 13 thread B blocks in `read`; a `SIGUSR1` with an `SA_RESTART` handler
    arrives (sent with `tgkill` to B); B's read keeps waiting and returns
    the value A writes afterwards; the handler ran once.
  - 14 same without `SA_RESTART`: the read fails `EINTR`.
  - 15 `poll` for `POLLOUT` on a fresh eventfd reports `POLLOUT`
    immediately; at `max` (counter `0xfffffffffffffffe`) it doesn't.
  - 16 `select` with the eventfd in the write set reports it writable.
  - 17 a `fork`ed child blocks in `read` and is `SIGKILL`ed while blocked;
    the parent's write and read still work afterwards (no lock is held
    during a blocking wait).
  - 18 lock-holder death: run only when `robust_mutex=ok` — with
    `LINUXEMU_TEST_EVENTFD_DIE_LOCKED=1` in the child's environment the
    core `_exit`s while holding the lock inside the next write; the parent's
    next read returns the counter value from before the write and
    `poll` agrees with it. Add the env hook in `src/linux_eventfd.c` under
    `#ifdef LINUXEMU_TESTING` only; the suite runs check 18 with
    `build/linuxemu-loader-test` (built with `-DLINUXEMU_TESTING=1`).
- [ ] **Step 2: Run on the Task 2 core.** Expected: exit 11 (blocking read
  fails `EAGAIN`).
- [ ] **Step 3: Implement blocking.** An empty read or full write on a
  blocking description waits in `poll()` on the readiness channel (read)
  or by sleeping on the channel with a short timeout ladder (write; the
  full case is rare) plus the thread's signal-wait fd, then re-checks under
  the lock. A signal: return `-EINTR`; the existing syscall-restart path
  applies `SA_RESTART` (check how native blocking calls get restarted and
  reuse it — don't add a second mechanism).
- [ ] **Step 4: Make `do_poll` and `do_select` honour `ready_now`.** For a
  typed fd whose `ready_now` intersects the requested events, set those
  `revents` and use a zero timeout for the native call.
- [ ] **Step 5: Lock-holder death.**
  - `robust_mutex=ok`: make the mutex robust; on `EOWNERDEAD`, recompute
    the channel from `counter` (fill or drain to match), then
    `pthread_mutex_consistent`.
  - `robust_mutex` absent: replace the mutex with a 64-bit compare-and-swap
    on `counter` (`__sync_val_compare_and_swap` on `uint64_t`; check it
    emits `ldrexd`/`strexd` and doesn't need libatomic) and treat the
    channel byte as a hint readers re-check. Drop check 18; check 17 still
    applies.
- [ ] **Step 6: Run the suite.** Expected: `eventfd-semantics` and
  `eventfd-blocking` exit 0; `linuxemu_event_smoke_failures=0`.
- [ ] **Step 7: Commit** ("Block, restart and recover in eventfd").

---

### Task 4: eventfd across `fork`, `exec` and `SCM_RIGHTS`

**Files:**
- Modify: `src/linux_eventfd.c`, `src/guest_fd.h`, `src/guest_fd.c`
  (multi-fd exec records; rights companions), `src/linux_socket.c`
  (`sendmsg`/`recvmsg` `SCM_RIGHTS` paths at about lines 290-410)
- Create: `guest-tests/eventfd-share.c`

**Interfaces:**
- Produces, in `struct guest_fd_ops` (append):
  `int (*companions)(struct guest_fd_object *, int fd, int *out, int max);`
  writes the hidden fds a receiver needs and returns how many.
- Produces in `src/guest_fd.h`:
  - `typedef int (*guest_fd_receive_fn)(int fd, const int *companions, int count);`
  - `void guest_fd_register_receive(enum guest_fd_type, guest_fd_receive_fn);`
- Exec record body for eventfd: `"shm:channel:"` (channel `-1` when the
  FIFO form has no hidden write end). The parser must still accept the
  old one-number body for other types.
- `SCM_RIGHTS` marker: only when at least one sent fd has companions, the
  sender appends, after the guest's fds, every companion fd and then one
  **descriptor-map fd**: a fresh unnamed shm page (created and unlinked
  for this message) holding `"LXRIGHT1"`, the guest fd count, and for each
  guest fd its object type and companion count. The receiver checks the
  last received fd: if it is shm, at least 16 bytes, and starts with the
  magic, it reads the map, calls the registered receive function per
  object, closes the map and the consumed companions, and trims the array
  and `msg_controllen` back to the guest's count. The guest sees exactly
  the fds and `msg_controllen` it would on Linux. A non-Linuxemu peer gets
  the extra fds (documented limit). Messages without such objects are sent
  unchanged.

- [ ] **Step 1: Write `guest-tests/eventfd-share.c`**:
  - 10 setup failed.
  - 11 after `fork`, the child writes 4 and exits; the parent reads 4.
  - 12 after `fork`, the child sets `O_NONBLOCK` with `F_SETFL`; the
    parent's `F_GETFL` shows it (shared description).
  - 13 the roadmap Phase 1 extra gate: the parent creates an eventfd and a
    pipe, `fork`s; the child closes its inherited copies of both and
    receives them over an `AF_UNIX` socketpair with `SCM_RIGHTS`; the child
    writes 6 to the eventfd and "x" into the pipe; the parent reads 6 and
    "x". The child checks the received message carried exactly 2 fds.
  - 14 `exec`: an eventfd without `EFD_CLOEXEC` holding 3 survives
    `execve` of the fixture itself (argv flag), and the new image reads 3,
    then writes 1 which the original parent (waiting on the shared
    counter after `fork`) sees.
  - 15 a received eventfd that is already open in the receiving process
    (sent to self over a socketpair) is the same object: a write through
    one fd is read through the other.
- [ ] **Step 2: Run on the Task 3 core.** Expected: exit 13 (the child's
  received eventfd is a bare FIFO/pipe; the write fails or isn't seen).
- [ ] **Step 3: Implement** companions and receive for eventfd; the
  multi-fd exec record; the marker and stripping in `sendmsg`/`recvmsg`;
  `fork` needs nothing beyond the inherited mapping (verify check 11).
- [ ] **Step 4: Run the suite plus `run-process-smoke.sh`** (its
  `fd-rights-adopt` and `fd-exec-carry` must stay green). Expected: all
  exit 0.
- [ ] **Step 5: Commit** ("Share eventfd across fork, exec and SCM_RIGHTS").

---

### Task 5: epoll core: create, ctl, level-triggered wait

**Files:**
- Create: `src/linux_epoll.c`, `guest-tests/epoll-basic.c`
- Modify: `src/linux_syscall.c` (dispatch 250, 251, 252, 346, 357, 441),
  `src/linuxemu.h`, `scripts/run-event-smoke.sh`

**Interfaces:**
- Consumes: `ready_now` (Task 2).
- Produces, in `src/linuxemu.h`:
  - `int32_t linux_epoll_create(uint32_t linux_flags);`
  - `int32_t linux_epoll_ctl(int epfd, int op, int fd, const void *event);`
  - `int32_t linux_epoll_wait(int epfd, void *events, int maxevents,
    int timeout_ms, const void *sigmask, uint32_t sigsetsize);`
    (`epoll_wait`, `epoll_pwait`, and `epoll_pwait2` after converting its
    timespec with the same rounding as `ppoll`'s `timespec_timeout`).
- Produces in `src/linux_epoll.c`: `void *epoll_storage_alloc(size_t)`,
  `void epoll_storage_free(void *, size_t)` — `mmap`-backed, used for item
  arrays and the per-wait `pollfd` array.
- Item and instance layout as in the spec ("Design: epoll"). The epoll
  fd's native backing is its wake channel (FIFO `O_RDWR` if
  `fifo_rdwr=ok`, else a pipe with a hidden write end).

- [ ] **Step 1: Write `guest-tests/epoll-basic.c`**:
  - 10 setup failed.
  - 11 `epoll_create1(0)` and `epoll_create(1)` succeed;
    `epoll_create1(EPOLL_CLOEXEC)` sets `FD_CLOEXEC`;
    `epoll_create1(1)` fails `EINVAL`; `epoll_create(0)` fails `EINVAL`.
  - 12 `epoll_ctl` errors: bad epfd `EBADF`; epfd not an epoll `EINVAL`;
    `fd == epfd` `EINVAL`; bad op `EINVAL`; regular file `EPERM`; second
    `ADD` `EEXIST`; `MOD`/`DEL` of an unregistered fd `ENOENT`; hidden fd
    (≥ 936) `EBADF`.
  - 13 pipe: no data → `epoll_wait(…, 0)` returns 0; after a write it
    returns 1 with `EPOLLIN` and the registered `data`
    `0x1122334455667788` intact; still reported on the next wait (level).
  - 14 `MOD` to `EPOLLOUT` on the pipe's write end reports `EPOLLOUT`; `DEL`
    then wait returns 0.
  - 15 eventfd registered: no event, then `EPOLLIN` after a write; with
    `EPOLLOUT` requested it's reported at once (`ready_now`).
  - 16 a socketpair whose peer is closed reports `EPOLLHUP` (and `EPOLLIN`)
    without being requested.
  - 17 timeout: `epoll_wait(…, 50)` with nothing ready returns 0 after
    ≥ 50 ms.
  - 18 `maxevents` 0 → `EINVAL`; with 3 ready fds and `maxevents` 2, two
    consecutive waits together report all three (rotation).
  - 19 `epoll_pwait2` with `{0, 30000000}` returns 0 after ≥ 30 ms.
  - 20 `epoll_pwait` with a mask blocking `SIGUSR1`: a `SIGUSR1` sent
    during the wait doesn't interrupt it; it's delivered after.
  - 21 a closed fd's registration is gone: register a pipe, close it,
    `epoll_wait(…, 0)` returns 0.
  - 22 the soft `RLIMIT_NOFILE` minus 20 fds registered (at least 700;
    pipe ends), data written to the middle one: one wait returns exactly
    that fd's `data`.
  - 23 `epoll_wait` with events `NULL` or an unmapped address fails
    `EFAULT` when an event is ready.
- [ ] **Step 2: Run on the Task 4 core.** Expected: exit 11 (`ENOSYS`).
- [ ] **Step 3: Implement** create/ctl/wait per the spec: items keyed by
  fd plus identity, dropped silently on mismatch; `EPERM` via `fstat`
  `S_ISREG`/`S_ISDIR`; `pollfd` array built from items with
  `guest_fd_poll_target` and translated events (reuse the translation in
  `src/linux_poll.c` — export it rather than copying); `ready_now`
  contributions; the wake channel in the set; rotation of the start index;
  `EFAULT` by validating the output range before writing (use the
  project's existing guest-pointer check if one exists, otherwise `msync`-
  probe the range — check `src/guest_memory.c` first).
- [ ] **Step 4: Run the suite.** Expected: `epoll-basic exit=0`.
- [ ] **Step 5: Commit** ("Add epoll create, ctl and level-triggered wait").

---

### Task 6: epoll under concurrency: ctl and close while another thread waits

**Files:**
- Modify: `src/linux_epoll.c`, `src/guest_fd.c` (close hook),
  `src/linux_syscall.c` (close paths, if any bypass `guest_fd_clear`)
- Create: `guest-tests/epoll-concurrent.c`

**Interfaces:**
- Produces: `void linux_epoll_note_close(int fd);` called wherever a guest
  fd number is closed or replaced: `close`, `dup2`/`dup3` onto it,
  `close_range`, close-on-exec at `exec`. It checks a global bitmap
  `epoll_watched[fd]` (set on `ADD`, recomputed on `DEL` and close) and
  wakes each epoll instance watching that fd.

- [ ] **Step 1: Write `guest-tests/epoll-concurrent.c`**:
  - 10 setup failed.
  - 11 thread B waits (timeout −1); thread A adds a ready pipe: B returns
    it within 200 ms.
  - 12 B waits on a pipe without data; A `MOD`s it to `EPOLLOUT` on the
    write end: B returns `EPOLLOUT`.
  - 13 B waits on a ready pipe that A `DEL`s first: B never reports it
    (A deletes, then B waits with 100 ms: 0).
  - 14 B waits; A closes a watched fd: B doesn't return an event for it.
  - 15 B waits; A `dup2`s a fresh pipe onto a watched fd number and writes
    to it: B reports nothing for that number (not registered).
  - 31 B waits on a listening socket and an idle pipe; A closes the pipe,
    a client connects, B `accept`s and gets the closed pipe's number;
    data arrives on the new socket: no event for the old item is ever
    reported (loop 200 times).
  - 32 stress: 4 threads add, modify, delete and close 64 fds for 5 s
    while 2 threads wait; no crash, no event with stale `data`
    (every reported `data` maps to a currently registered fd).
- [ ] **Step 2: Run on the Task 5 core.** Expected: exit 11 (B sleeps
  through the add) — or the first check that fails; record it.
- [ ] **Step 3: Implement** the wake on `ctl` (byte into the wake channel,
  bump `generation`, waiter drains and rebuilds with the remaining
  timeout) and `linux_epoll_note_close` in every close path. Use the
  `poll_close_race` finding from Task 1: if QNX `poll()` returns `POLLNVAL`
  or the new fd's events for a number closed under it, the waiter must
  discard results for items whose identity changed.
- [ ] **Step 4: Run the suite.** Expected: `epoll-concurrent exit=0`
  three runs in a row.
- [ ] **Step 5: Commit** ("Make epoll safe against concurrent ctl and close").

---

### Task 7: `EPOLLET`, `EPOLLONESHOT`, `EPOLLRDHUP`, `EPOLLEXCLUSIVE`

**Files:**
- Modify: `src/linux_epoll.c`, `src/linux_syscall.c` and
  `src/linux_socket.c` (I/O re-arm hooks), `src/linux_file_extra.c`
  (`preadv`/`pwritev`/`sendfile64` hooks)
- Create: `guest-tests/epoll-modes.c`

**Interfaces:**
- Produces: `void linux_epoll_note_io(int fd);` — an inline-cheap check of
  the `epoll_et_watched[fd]` bitmap, then clears the quiet flag on the
  edge-triggered items for that fd. Called after `read`, `readv`, `pread*`,
  `recv*`, `write`, `writev`, `send*`, `accept*`, `sendfile64`.

- [ ] **Step 1: Write `guest-tests/epoll-modes.c`**:
  - 10 setup failed.
  - 11 `EPOLLET` pipe: write → one event; second wait (0) → none; read
    part of the data → next wait reports again (re-armed by I/O); read to
    `EAGAIN` and write again → reported.
  - 12 `EPOLLONESHOT`: one event, then none even with data; `MOD` re-arms
    and the next wait reports.
  - 13 `EPOLLRDHUP` on a TCP loopback pair: peer `shutdown(SHUT_WR)` →
    `EPOLLRDHUP|EPOLLIN`; without `EPOLLRDHUP` requested it isn't reported.
  - 14 same over an `AF_UNIX` stream pair.
  - 15 `EPOLLEXCLUSIVE` with `ADD` succeeds; with `MOD` fails `EINVAL`;
    combined with `EPOLLONESHOT` fails `EINVAL`.
  - 16 `EPOLLWAKEUP` is accepted and ignored.
  - 17 an ET eventfd (libuv's async pattern): write 1 → event; read →
    no event; write 1 → event.
- [ ] **Step 2: Run on the Task 6 core.** Expected: exit 11.
- [ ] **Step 3: Implement** quiet/disabled flags; the I/O hooks; RDHUP
  detection per the `rdhup_signal` finding from Task 1 (prefer a host
  `revents` bit; fall back to a zero-length `recv(MSG_PEEK|MSG_DONTWAIT)`
  only for sockets with `EPOLLRDHUP` requested and `POLLIN` set);
  `EPOLLEXCLUSIVE` validation as Linux.
- [ ] **Step 4: Run the suite.** Expected: `epoll-modes exit=0`.
- [ ] **Step 5: Commit** ("Add edge-triggered, one-shot and RDHUP epoll modes").

---

### Task 8: Nested epoll, epoll in `poll`/`select`, `fork`, `exec`, and the Looper pattern

**Files:**
- Modify: `src/linux_epoll.c`, `src/linux_poll.c`
- Create: `guest-tests/epoll-nesting.c`, `guest-tests/looper-pattern.c`

**Interfaces:**
- Produces:
  - `int linux_epoll_expand(struct guest_fd_object *ep, struct pollfd *out,
    int capacity, int depth);` appends the host `pollfd`s for an epoll's
    enabled items (recursively); returns the count, or −1 with `ELOOP`
    past depth 5 or `ENOMEM` past `capacity`.
  - `int linux_epoll_expanded_ready(struct guest_fd_object *ep,
    const struct pollfd *in, int count);` 1 if any expanded item is ready
    (after `ready_now` and identity checks), without consuming `ET`/
    `ONESHOT` state.
  - A `fork` hook copying each epoll instance's items (objects gain a
    reference); an exec record carrying the item list when it fits.

- [ ] **Step 1: Write `guest-tests/epoll-nesting.c`**:
  - 10 setup failed.
  - 11 epoll A watches epoll B, which watches a pipe; a write to the pipe
    makes A report B with `EPOLLIN`; B's own wait then reports the pipe.
  - 12 `poll` on epoll B reports `POLLIN` when its pipe has data, nothing
    otherwise; `select` agrees.
  - 13 cycles: adding A to B after B is in A fails `ELOOP`; a chain of 6
    nested epolls fails `ELOOP` at the 6th `ADD`.
  - 14 an `EPOLLONESHOT` item inside B is still reported by B's own wait
    after A was woken by it (probing doesn't consume).
  - 15 `fork`: the child's epoll still reports the inherited pipe; a
    child-side `DEL` doesn't affect the parent's wait.
  - 16 `exec` without `EPOLL_CLOEXEC`: the new image's epoll reports the
    inherited pipe (or, if the list didn't fit, the spec's documented
    limit applies and the test checks the instance is valid and empty —
    with 3 items it must fit).
- [ ] **Step 2: Write `guest-tests/looper-pattern.c`** (roadmap Android
  addition): one epoll watching an eventfd wake plus 2 pipes and a
  socketpair, level-triggered; the main thread loops `epoll_wait(…, 100)`;
  a second thread wakes it through the eventfd 1000 times while a third
  adds and removes the pipes; checks: 11 every wake is observed (counter
  reads sum to 1000), 12 no event names a removed fd, 13 the loop exits
  within 2 s of a final "quit" byte.
- [ ] **Step 3: Run both on the Task 7 core.** Expected: nesting exit 11;
  Looper passes or fails — record which (it may already pass).
- [ ] **Step 4: Implement** expansion in `do_poll`, `do_select` (within
  `FD_SETSIZE`) and epoll-in-epoll; the fork copy; the exec record.
- [ ] **Step 5: Run the suite.** Expected: all event fixtures exit 0;
  `linuxemu_event_smoke_failures=0`.
- [ ] **Step 6: Commit** ("Nest epoll and carry it across fork and exec").

---

### Task 9: Part 1 gate and docs

**Files:**
- Modify: `docs/compatibility.md`, `docs/FINDINGS.md`,
  `docs/corpus-results.md`, `docs/capability-roadmap.md` (Phase 3 status:
  part 1 complete, carry-overs), `docs/development.md` (new suite)

- [ ] **Step 1: Full regression** on the device with `ssh -tt`.
  Run: `sh scripts/run-full-regression.sh`
  Expected: every suite `=ok`, `linuxemu_full_regression_failures=0`.
  (If `run-python-smoke` hits the known `0x60067dc4` crash, record the run
  and rerun once; it's a watch item, not this phase's gate.)
- [ ] **Step 2: Corpus.**
  Run: `sh scripts/run-corpus.sh`
  Expected: compatible count ≥ 7; Node past `epoll_create1` with its next
  blocker named; nginx and Redis "basic" pass or a new blocker named with
  the failing call.
- [ ] **Step 3: Docs.** Every documented limit from the spec goes into
  `docs/compatibility.md`: fork-copied epoll, the `dup`-outlives-original
  case, the `EPOLLET` approximation, `EPOLLEXCLUSIVE` waking all waiters,
  near-full eventfd `POLLOUT` not waking sleepers, epoll fds over
  `SCM_RIGHTS` arriving as plain fds, the per-process object capacity, and
  a non-Linuxemu peer receiving companion fds. Findings from Tasks 1–8 go
  into `docs/FINDINGS.md`.
- [ ] **Step 4: Commit** ("Record the Phase 3 part 1 gate").
