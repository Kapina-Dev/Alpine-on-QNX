# Phase 3, part 3: inotify — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Linux `inotify` as a Phase 1 descriptor-layer object, backed by
QNX's native inotify, so inotify-tools becomes compatible.

**Architecture:** The guest's inotify fd *is* QNX's native inotify fd
(`inotify_init()` through `/dev/fsnotify`), carrying a stateless
`GUEST_FD_INOTIFY` object. Linuxemu translates guest paths, validates
masks and flags, turns the native nonblocking empty read (0) into
`EAGAIN`, and maps unsupported filesystems (`ENOTTY`) to `EOPNOTSUPP`.
Readiness, blocking, `dup`, `fork` and `exec` come from the native fd.
Carried and received fds are recognised by `/dev/fsnotify`'s identity.
QNX can't pass an inotify fd over `SCM_RIGHTS`, so `sendmsg` refuses one.
New file `src/linux_inotify.c`.

**Tech Stack:** C (gnu99), BB10 SDK gcc 4.6.3 on the Passport; C guest
fixtures (musl, pthreads) via `scripts/build-guest-fixture.sh`; native QNX
probes in `native-probes/`.

**Spec:** `docs/superpowers/specs/2026-11-20-phase3c-inotify-design.md`
(the authority for every semantic and every device fact; this plan only
adds decisions and pinned values).

## Global Constraints

- **Backend:** the native QNX `inotify_init`/`inotify_add_watch`/
  `inotify_rm_watch` from libc (`<sys/inotify.h>`). No relay thread, no
  emulated queue, no hidden fds, no polling, no stat-scanning fallback.
- **The object holds no state.** Queue, watches and `O_NONBLOCK` live on
  the native open-file description. Every inotify fd has `/dev/fsnotify`'s
  identity, so adopting another inotify fd's object must stay harmless.
- **Proper errors, never fake success:** unsupported cases return a Linux
  error and go through `linux_report_unsupported(number, linux_error,
  detail, args)` so they show in `LINUXEMU_DIAGNOSTICS`. Never a watch that
  silently never fires.
- **Descriptor rules:** a guest fd that `guest_fd_is_hidden()` reports →
  `EBADF`; new descriptors through `guest_fd_admit()`.
- **Signal safety:** no `malloc`/`calloc`/`free`/stdio in dispatch paths
  (they run inside the SIGILL handler); object creation through
  `guest_fd_object_new` is the accepted exception.
- **Lock order** from part 1: fd_replace → registry → instance → table.
  This part adds no locks. If one turns out to be needed, it must be a leaf;
  say so in the report.
- **Builds are warning-free** (`-Wall -Wextra -O2 -std=gnu99 -marm`).
- **Tests:**
  - C fixtures exit with a distinct code per check (10, 11, …) and print
    the failing check and errno on stderr.
  - Every wait is bounded by a watchdog that exits 100+N (BerryCore
    `timeout` never fires).
  - Events arrive about 250 ms late (QNX batching), so wait up to 2 s per
    expected event with `poll`.
  - Watch only the bits under test, or skip `OPEN`/`ACCESS`/
    `CLOSE_NOWRITE` from Passport background processes.
  - Fixture directories are under the guest's `/tmp` (rootfs `tmp`, qnx6).
  - Every new test fails on the pre-phase core and passes on the new one:
    before Task 2, save the device's `build/linuxemu` as
    `build/linuxemu-pre-phase3c`.
- **Device:**
  - The Passport is at `ssh -o HostName=<address> bb10` (DHCP may move
    it; ask the user if it's unreachable). Never write the IP into the
    repo.
  - Dev tree `/accounts/1000/shared/misc/linuxemu-dev` (no git). Sync with
    `tar -cf - src scripts guest-tests native-probes tests | ssh … 'cd DEV && tar -xf -'`,
    without `-tt`.
  - Build with `sh scripts/build-native-probes.sh`.
  - Fixtures: `ALPINE_ROOT=build/fixture-rootfs sh scripts/build-guest-fixture.sh NAME`;
    commit `guest-tests/fixtures/NAME-armhf` and add it to
    `scripts/run-event-smoke.sh` with `run_rootfs_fixture NAME`.
  - `run-full-regression.sh` needs `ssh -tt` and no device-side pipes.
  - **Do not run `scripts/run-iopkt-stress.sh`.**
- **Commits:**
  - Work in worktree branch `phase3c-inotify`, one commit per task with
    device evidence (exit codes before and after).
  - End each message with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`
    and no `Claude-Session:` trailer.
  - Never stage `root-analysis/native-probe-results-20260922.md`,
    `native-probes/unix-rights-pressure.c` or
    `root-analysis/terminal-debug-20261014/`.
- **ARM EABI numbers and constants:**

  | Item | Value |
  | --- | --- |
  | inotify_init, inotify_add_watch, inotify_rm_watch | 316, 317, 318 |
  | inotify_init1 | 360 |
  | IN_NONBLOCK, IN_CLOEXEC (Linux O_NONBLOCK, O_CLOEXEC) | 0x800, 0x80000 |
  | IN_ALL_EVENTS (event bits) | 0x00000fff |
  | IN_ONLYDIR, IN_DONT_FOLLOW, IN_EXCL_UNLINK | 0x01000000, 0x02000000, 0x04000000 |
  | IN_MASK_CREATE, IN_MASK_ADD, IN_ISDIR, IN_ONESHOT | 0x10000000, 0x20000000, 0x40000000, 0x80000000 |
  | IN_Q_OVERFLOW, IN_IGNORED | 0x4000, 0x8000 |
  | Linux EOPNOTSUPP | 95 |
  | sendmsg, sendmmsg (for diagnostics) | 296, 374 |
  | FIONREAD (Linux) | 0x541b |

  The QNX header uses the same values, so masks pass through unchanged.
  `struct inotify_event` is `{s32 wd; u32 mask; u32 cookie; u32 len;
  char name[len]}`, the same on both sides.

## Review Focus

1. **Relative paths and guest symlinks:** `inotify_add_watch(fd, ".")`
   after `chdir`, and a guest absolute symlink inside the rootfs, must
   watch the guest's target, not a host path (with `IN_DONT_FOLLOW`, the
   link itself). Test: Task 2 checks 24 and 25.
2. **Bad guest pointers:** an invalid path pointer → `EFAULT`; an invalid
   read buffer → `EFAULT`, not a crash. Test: Task 2 check 26.
3. **A burst from several writers** (the corpus "concurrent" stage): 4
   threads creating 25 files each; all 100 `CREATE` events arrive, no
   overflow, none duplicated. Test: Task 3 check 18.
4. **fd number reuse under epoll:** close an inotify fd registered in
   epoll, open a pipe on the same number. The epoll set must not report
   the old inotify readiness. Test: Task 3 check 19.
5. **The read buffer size inotify-tools uses** (`sizeof(struct
   inotify_event) + NAME_MAX + 1` per event, several events queued): every
   read returns whole records and the loop consumes all of them. Test:
   Task 2 check 27.

---

### Task 1: Commit the probes and record the device facts

**Files:**
- Add: `native-probes/phase3c-probes.c`, `native-probes/phase3c-facts.c`
  (already written, uncommitted on master; move them into the worktree)
- Modify: `scripts/build-native-probes.sh` (build both; `phase3c-facts`
  links `-lsocket`; add both to the trailing `readelf -h` list), `docs/FINDINGS.md`

- [ ] **Step 1:** Add both probes to `scripts/build-native-probes.sh`
  after `phase3b-probes`.
- [ ] **Step 2: Build and run on the device.**
  - Run `sh scripts/build-native-probes.sh`, then from `build/`:
    `native-probes/phase3c-probes $PWD/inotify-probe.d /dev/shmem/x`,
    `native-probes/phase3c-probes $PWD/inotify-probe.d /dev/shmem/x more`
    and `native-probes/phase3c-facts $PWD/inotify-facts.d`.
  - Expected: each ends `probe_done=1`, with no `probe_guard`. The values
    match the spec's "What the device probe found" and "Follow-up facts"
    (latency numbers may differ).
- [ ] **Step 3: Record in `docs/FINDINGS.md`.** Add a section "QNX native
  inotify (Phase 3 part 3 probes)" with the date, device, user, what was
  probed, the results, and the evidence lines. Label each value as
  measured. Cover:
  - access and records;
  - the five differences from Linux;
  - identity, exec, dup and the `SCM_RIGHTS` `EACCES`;
  - the overflow record's position;
  - the cost of scanning;
  - `ionotify` returning `EINVAL`.
- [ ] **Step 4: Commit** ("Probe QNX native inotify for Phase 3 part 3").

---

### Task 2: The inotify object and the four syscalls

**Files:**
- Create: `src/linux_inotify.c`, `guest-tests/inotify-basic.c`
- Modify: `src/linuxemu.h` (declarations), `src/linux_syscall.c` (cases
  316, 317, 318, 360; call `linux_inotify_register()` next to
  `linux_signalfd_register()`), `scripts/run-event-smoke.sh`

**Interfaces:**
- Produces (in `src/linuxemu.h`):
  - `int32_t linux_inotify_init(uint32_t linux_flags);` (316 passes 0)
  - `int32_t linux_inotify_add_watch(int fd, const char *guest_path, uint32_t mask);`
  - `int32_t linux_inotify_rm_watch(int fd, int32_t wd);`
  - `int linux_inotify_is_native(int fd);`: 1 when `fstat64(fd)` matches
    `/dev/fsnotify`'s `st_dev`/`st_ino` (recorded at registration), else 0.
    Used by Tasks 4 and 5.
  - `int linux_inotify_attach(int fd);`: attaches a fresh inotify object
    to fd (0 or -1 with errno). Used by Task 4.
  - `void linux_inotify_register(void);`

- [ ] **Step 1: Write `guest-tests/inotify-basic.c`.** Each check below is
  its own exit code:
  - 10: setup failed.
  - 11: `inotify_init1(IN_NONBLOCK)` + `read` on an empty queue → -1/`EAGAIN`.
  - 12: `inotify_init1(IN_CLOEXEC)` → `fcntl(F_GETFD) & FD_CLOEXEC`.
  - 13: `inotify_init1(0x1)` → -1/`EINVAL`.
  - 14: dir watch `IN_CREATE|IN_MODIFY|IN_CLOSE_WRITE|IN_ATTRIB|IN_DELETE`:
    create+write+close of `f` gives `CREATE f`, `MODIFY f`,
    `CLOSE_WRITE f`.
  - 15: `chmod f` gives `ATTRIB f`; `unlink f` gives `DELETE f`.
  - 16: `rename a b` with `IN_MOVE` gives `MOVED_FROM a` and `MOVED_TO b`
    with the same nonzero cookie.
  - 17: `rmdir` of the watched dir gives `DELETE_SELF` then `IGNORED` (same wd).
  - 18: `inotify_rm_watch` gives `IGNORED`; a second `rm_watch` of that wd
    gives -1/`EINVAL`.
  - 19: mask 0 → -1/`EINVAL`; `IN_MASK_ADD|IN_MASK_CREATE|IN_CREATE` →
    -1/`EINVAL`; `IN_MASK_CREATE|IN_CREATE` → -1/`EINVAL`.
  - 20: missing path → -1/`ENOENT`; `IN_ONLYDIR` on a file → -1/`ENOTDIR`.
  - 21: `add_watch` on a pipe fd → -1/`EINVAL`; on fd 999 (closed) →
    -1/`EBADF`.
  - 22: `read` with a 16-byte buffer while a named event is queued →
    -1/`EINVAL`.
  - 23: `ioctl(FIONREAD)` with one `CREATE x` queued → 32 (16 + len 16).
  - 24: `chdir(dir)`, `inotify_add_watch(fd, ".", IN_CREATE)`; create `r`
    → `CREATE r`.
  - 25: a guest symlink `/tmp/inb-link -> /tmp/inb-dir` (absolute guest
    target): a watch through the link reports `CREATE` in the target. With
    `IN_DONT_FOLLOW|IN_ATTRIB`, creating a file in the target reports
    nothing within 1 s.
  - 26: `add_watch` with path `(char *)1` → -1/`EFAULT`; `read` into
    `(void *)1` with an event queued → -1/`EFAULT`.
  - 27: 20 files created, then a read loop with a
    `sizeof(struct inotify_event) + NAME_MAX + 1` buffer consumes exactly
    20 `CREATE` records; every read returns whole records. One more
    event read with `readv` (two iovecs, 16 + 256 bytes) gives the same
    record.
  - 28: `add_watch` on a guest path that resolves to a native device
    (`/dev/pts/N` of the fixture's own terminal if any) → -1/`EOPNOTSUPP`
    when the native call says `ENOTTY`. If no such path is reachable,
    print `skip 28` and continue (spec: the mapping may be unreachable from
    the guest).
- [ ] **Step 2: Build the fixture and run it on `build/linuxemu-pre-phase3c`.**
  Expected: exit 10 (`inotify_init` `ENOSYS`).
- [ ] **Step 3: Implement `src/linux_inotify.c`.**
  - `linux_inotify_init`: reject flags outside `0x800|0x80000` with
    `EINVAL`; native `inotify_init()`; `guest_fd_admit`; apply the flags
    with `fcntl`; `linux_inotify_attach`.
  - `linux_inotify_add_watch`:
    - fd checks (hidden or closed → `EBADF`; no inotify object → `EINVAL`);
    - mask checks per the spec (event bits `& 0xfff` == 0 → `EINVAL`;
      `IN_MASK_CREATE` set → `EINVAL`), passing the other bits through;
    - path through `guest_path_resolve(path, 0, …)`, or
      `guest_path_resolve_nofollow` when `IN_DONT_FOLLOW`;
    - a native `ENOTTY` becomes `EOPNOTSUPP` via `linux_report_unsupported(317, 95, "inotify: filesystem has no change notification", args)`.
  - Read op: native `read`. A return of 0 → `-EAGAIN`. Then
    `linux_epoll_note_io(fd)`.
  - `poll_fd` returns fd; no `ready_now`, `companions` or status ops.
  - `serialize` writes the exec-record body for "no hidden fd, empty
    path" (hidden -1, no hex digits). Check the exact text against
    `restore_one`'s parser in `src/guest_fd.c`. The restore and receive
    hooks are added in Task 4.
- [ ] **Step 4: Run** `run-event-smoke.sh`. Expected: `inotify-basic
  exit=0` and every earlier fixture still exit 0.
- [ ] **Step 5: Commit** ("Add inotify on QNX's native inotify").

---

### Task 3: Readiness, blocking and bursts

**Files:**
- Create: `guest-tests/inotify-modes.c`
- Modify: `scripts/run-event-smoke.sh`; `src/linux_inotify.c`,
  `src/linux_poll.c` or `src/linux_epoll.c` only if a check fails

**Interfaces:**
- Consumes: Task 2's syscalls.

- [ ] **Step 1: Write `guest-tests/inotify-modes.c`:**
  - 10: setup failed.
  - 11: `IN_ONESHOT|IN_CREATE`: two creates give one `CREATE`, then
    `IGNORED`, and nothing more within 1 s.
  - 12: `IN_CREATE` watch, then `IN_DELETE|IN_MASK_ADD` on the same path →
    the same wd; create and delete give both events.
  - 13: re-adding without `IN_MASK_ADD` returns the same wd and replaces
    the mask (`IN_DELETE` only: a create gives nothing within 1 s, a
    delete gives `DELETE`).
  - 14: overflow: 1500 create+unlink pairs with no read, then drain
    nonblocking. An `IN_Q_OVERFLOW` record with wd -1 and len 0 appears
    somewhere in the stream (QNX puts it first).
  - 15: `poll`, `select` and `ppoll` report the fd readable within 2 s of a
    create, and not readable once drained.
  - 16: epoll level-triggered reports `EPOLLIN` until drained.
    Edge-triggered reports once per burst, and again after a read plus a
    new event.
  - 17: a blocking `read` in thread B is woken by a create from thread A;
    a blocking `read` interrupted by a `SIGUSR1` handler without
    `SA_RESTART` returns -1/`EINTR`.
  - 18: 4 threads create 25 distinct files each in the watched dir. All
    100 names arrive exactly once within 5 s, with no `IN_Q_OVERFLOW`.
  - 19: an inotify fd in an epoll set is closed, and a pipe is created on
    the same number (`dup2`); `epoll_wait(…, 0)` reports nothing for that
    number until the pipe is written.
- [ ] **Step 2: Run on the pre-phase core** (expected exit 10) and on the
  Task 2 core. Record which checks fail.
- [ ] **Step 3: Fix** whatever failed, in the file that owns the
  behaviour. If nothing fails, say so in the commit; the fixture is the
  deliverable.
- [ ] **Step 4: Run** `run-event-smoke.sh` three times. Expected:
  `inotify-modes exit=0` each time, all other fixtures 0.
- [ ] **Step 5: Commit** ("Test inotify readiness, blocking and bursts").

---

### Task 4: dup, fork, exec and SCM_RIGHTS

**Files:**
- Create: `guest-tests/inotify-share.c`
- Modify: `src/linux_inotify.c` (restore and receive hooks),
  `src/guest_fd.c` (`guest_fd_received` fallback), `src/linux_socket.c`
  (refuse inotify fds in `SCM_RIGHTS`), `scripts/run-event-smoke.sh`

**Interfaces:**
- Consumes: `linux_inotify_is_native(int fd)` and
  `linux_inotify_attach(int fd)` from Task 2.

- [ ] **Step 1: Write `guest-tests/inotify-share.c`:**
  - 10: setup failed.
  - 11: `dup` and `dup3(…, O_CLOEXEC)` copies read from the same queue
    (an event read through the copy is gone from the original), and
    `O_NONBLOCK` set through the copy makes an empty read on the original
    -1/`EAGAIN`.
  - 12: a forked child reads a `CREATE` produced by the parent's watch
    after the fork, and the parent then finds the queue empty.
  - 13: without `IN_CLOEXEC`, the fd survives `execve` of the fixture
    itself (argv mode `--exec FD DIR`). The new image creates a file and
    reads its `CREATE`. Its `add_watch` on the fd succeeds (proves it's
    typed, not native-only), and a nonblocking empty read gives `EAGAIN`.
  - 14: `IN_CLOEXEC` fd: after `execve`, `fcntl(fd, F_GETFD)` → -1/`EBADF`.
  - 15: `sendmsg` with the inotify fd in `SCM_RIGHTS` over a socketpair →
    -1/`EOPNOTSUPP`. A following plain `send` of `"ok"` arrives intact at
    the peer, and a `sendmsg` with a pipe fd still works (the peer reads
    from it).
- [ ] **Step 2: Run on the Task 3 core.** Expected: checks 13 and 15 fail
  (record the codes).
- [ ] **Step 3: Implement:**
  - `guest_fd_register_restore(GUEST_FD_INOTIFY, …)`: restore calls
    `linux_inotify_attach(fd)` after checking `linux_inotify_is_native(fd)`.
  - In `guest_fd_received`: when `adopt_received` finds nothing and
    `linux_inotify_is_native(fd)`, attach. Keep `guest_fd.c` free of
    inotify internals: call through a hook registered by
    `linux_inotify_register()`.
  - In `src/linux_socket.c`, before `guest_fd_rights_extra`, if any guest
    fd in the message has an inotify object or `linux_inotify_is_native`
    is true, return `-EOPNOTSUPP` through
    `linux_report_unsupported(296 or 374, 95, "inotify fd in SCM_RIGHTS: QNX cannot pass it", args)`.
    This covers `sendmsg` and `sendmmsg`.
- [ ] **Step 4: Run** event, process and network smoke. Expected: 0
  failures; `inotify-share exit=0` three times.
- [ ] **Step 5: Commit** ("Carry inotify across dup, fork and exec; refuse
  it in SCM_RIGHTS").

---

### Task 5: Corpus, gate and docs

**Files:**
- Modify: `scripts/corpus/apps/inotify-tools.sh` (header comment only),
  `docs/corpus-results.md`, `docs/compatibility.md`, `docs/FINDINGS.md`,
  `docs/capability-roadmap.md`, `docs/development.md`

- [ ] **Step 1: Corpus.** `sh scripts/run-corpus.sh` (all apps). Expected:
  inotify-tools `startup=pass basic=pass concurrent=pass shutdown=pass
  compatible=yes`. Every app compatible before this part (the 7 baseline
  apps plus Redis) is still compatible. If inotify-tools fails a stage,
  debug it (superpowers:systematic-debugging) before going on; a fix
  belongs to the task that owns the code.
- [ ] **Step 2: Full regression** with `ssh -tt`:
  `sh scripts/run-full-regression.sh`. Expected:
  `linuxemu_full_regression_failures=0`.
- [ ] **Step 3: Docs.**
  - `docs/compatibility.md`: a section "Limits of the Phase 3 part 3
    calls" listing:
    - unsupported filesystems give `EOPNOTSUPP` (`/dev/shmem`, `/pps`,
      `/proc`);
    - events arrive about 250 ms late;
    - the overflow record comes first in the queue;
    - extra events from Passport background processes;
    - `IN_MASK_CREATE` gives `EINVAL`;
    - inotify fds can't pass over `SCM_RIGHTS` (`EOPNOTSUPP`);
    - `IN_EXCL_UNLINK` is untested;
    - no `/proc/sys/fs/inotify` files.
  - `docs/corpus-results.md`: the new inotify-tools row and totals.
  - Roadmap: add "Phase 3 part 3 status" in the style of part 2, mark
    inotify done in the dependency chart, and state that Phase 3's libuv
    gate item (not run, the user's choice) and its nginx and Node.js items
    (non-inotify blockers) stay open.
  - `docs/development.md`: the new fixtures and probes.
- [ ] **Step 4: Commit** ("Record the Phase 3 part 3 gate").
