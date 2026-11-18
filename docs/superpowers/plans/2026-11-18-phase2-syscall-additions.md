# Phase 2: Contained Syscall Additions Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the Phase 2 Linux calls (the roadmap list, its Android
additions, and the calls the application corpus hits) plus the Node.js
`sigaction` fix. Each call is either Linux-correct or returns a deliberate
Linux error. Every existing regression stays green.

**Architecture:**
- Five groups, landing in order: Node, descriptors and file I/O, file
  metadata, sockets, process and resources.
- Most calls translate Linux structs and flags and then call the QNX
  equivalent. `preadv`/`pwritev(2)` and `sendfile64` are emulated with
  loops, because QNX has no such calls.
- New code goes in two new translation units:
  - `src/linux_file_extra.c` for Groups 2–3;
  - `src/linux_resource.c` for Group 5.
  `src/linux_syscall.c` only dispatches to them. The build already
  compiles `src/*.c`.

**Tech Stack:**
- C (gnu99) with the BB10 SDK gcc 4.6.3, building on the Passport.
- ARM assembly guest tests (`guest-tests/*.S`, linked with
  `minimal-arm.ld`).
- C guest fixtures, built with the guest's gcc by
  `scripts/build-guest-fixture.sh`.

**Spec:** `docs/superpowers/specs/2026-11-18-phase2-syscall-additions-design.md`.
Its "Scope and behaviour" section is the authority for every call's
semantics. This plan doesn't repeat it, only the values a test pins.

## Global Constraints

- **Proper errors, never fake success.** Unsupported cases return the
  spec's Linux error through `linux_unsupported(number, error, "detail",
  args)` (`src/linux_syscall.c:284`), so they show in
  `LINUXEMU_DIAGNOSTICS`. New translation units call it through a
  non-static wrapper (Task 1 exports it).
- **Every flag is translated explicitly.** Unknown Linux bits get EINVAL.
  No raw Linux constant is ever passed to QNX.
- **Phase 1 descriptor rules hold for every fd-taking call.**
  - A guest fd that `guest_fd_is_hidden()` reports → EBADF.
  - New descriptors go through `guest_fd_admit()`/`guest_fd_clear()`.
  - Dups go through `guest_fd_duplicate()`.
- **Signal safety.** No `malloc` and no stdio in the new dispatch paths,
  because they run inside the SIGILL handler. Use fixed stack buffers
  (for example a 16 KiB `sendfile64` buffer).
- **Builds are warning-free** under the device flags
  (`-Wall -Wextra -O2 -std=gnu99 -marm`).
- **Every test must fail on the pre-phase core and pass on the new one.**
  - The pre-phase core is `build/linuxemu` at sha256 `5089cc4c…`. Copy it
    to `build/linuxemu-pre-phase2` on the device before Task 1.
  - Show the failing run before the fix.
- **Tests:**
  - Assembly tests return a distinct exit code per check (10, 11, 12, …),
    so a failure names its check.
  - C fixtures do the same and print the failing check on stderr.
- **Phase order:** no `epoll`, `inotify`, `timerfd`, `signalfd` or full
  `eventfd` work.
- **Device:** the Passport, as `ssh -o HostName=<passport-ip> bb10`.
  - The IP changes; read it from the user's notes and never write it into
    the repo.
  - The dev tree is `/accounts/1000/shared/misc/linuxemu-dev` and has no
    git. Sync from the worktree without `-tt`:
    `tar -cf - src scripts guest-tests tests | ssh … 'cd DEV && tar -xf -'`.
  - Build with `sh scripts/build-native-probes.sh`.
  - `run-full-regression.sh` needs `ssh -tt` with no pipes or redirects.
  - BerryCore `timeout` never fires.
- **C fixtures:**
  - Build with
    `ALPINE_ROOT=build/fixture-rootfs sh scripts/build-guest-fixture.sh NAME`
    on the device.
  - Commit the resulting `guest-tests/fixtures/NAME-armhf`.
  - Fixtures that exec themselves run through `run_rootfs_fixture`
    (`scripts/run-process-smoke.sh:78`).
- **New `.S` tests** are added to the guest list in
  `scripts/build-native-probes.sh:76-85` and to the runner the task names.
- **Commits:**
  - Work in the worktree branch `phase2-syscalls` (created with
    superpowers:using-git-worktrees).
  - One commit per task.
  - The message records the device evidence (exit codes before and after).
  - No `Claude-Session:` trailer; end with the
    `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` line.
- **Never stage the user's files:** `docs/capability-roadmap.md`,
  `root-analysis/native-probe-results-20260922.md`, `ANDROID6_PLAN.md`,
  `native-probes/unix-rights-pressure.c`,
  `root-analysis/terminal-debug-20261014/`. The worktree doesn't contain
  their edits, so never copy those files, or `src/linux_socket.c`, from
  the main checkout.
- **ARM EABI numbers:**

  | Calls | Numbers |
  | --- | --- |
  | rt_sigaction, kill, tkill, tgkill | 174, 37, 238, 268 |
  | rt_sigqueueinfo, rt_tgsigqueueinfo | 178, 363 |
  | cacheflush | 0x0f0002 |
  | preadv, pwritev, preadv2, pwritev2 | 361, 362, 392, 393 |
  | sendfile64, fallocate, fcntl64 | 239, 352, 221 |
  | statx, statfs64, fstatfs64 | 397, 266, 267 |
  | fchmod, faccessat2 | 94, 439 |
  | chown32, lchown32, fchown32, fchownat | 212, 198, 207, 325 |
  | mknod, mknodat | 14, 324 |
  | sendmmsg, recvmmsg | 374, 365 |
  | setrlimit, ugetrlimit, getrusage, sysinfo | 75, 191, 77, 116 |
  | setitimer, getitimer | 104, 105 |
  | getpriority, setpriority | 96, 97 |
  | sched_setparam, sched_getparam, sched_setscheduler | 154, 155, 156 |
  | sched_setaffinity, sched_getaffinity | 241, 242 |
  | capget, capset, prctl | 184, 185, 172 |
  | membarrier, mremap | 389, 163 |

## Review Focus

These are the five failure modes most likely to bite a user that the
per-call tests wouldn't otherwise exercise. Each one has a test in the task
named.

1. **A NULL output pointer** (`statx`, `sysinfo`, `getrusage`, `capget`
   data, `sched_getaffinity`, `getitimer`) must return EFAULT, not crash
   Linuxemu. Tests are in Tasks 6, 9 and 10.
2. **A descriptor in the hidden range** passed to any new fd-taking call
   must return EBADF. That covers `preadv`, both `sendfile64` fds,
   `fallocate`, `fchmod`, `statx` with `AT_EMPTY_PATH`, and `sendmmsg`.
   Tests are in Tasks 4, 5, 6, 7 and 8.
3. **64-bit offsets beyond 4 GiB, and negative offsets,** in the split
   register pairs. `preadv` takes the position in r3/r4 (low, high);
   `fallocate` takes the offset in r2/r3 and the length in r4/r5. Above
   4 GiB the call reaches QNX intact or fails as QNX does; a negative
   value gets EINVAL. Tests are in Tasks 4 and 5.
4. **A signal arriving mid-transfer** in `preadv` on a pipe, `sendfile64`
   or `recvmmsg`:
   - after some bytes or messages moved: return the partial count;
   - before any moved, with `SA_RESTART`: restart the call.
   Add 239, 361, 362, 365, 374, 392 and 393 to `restartable_syscall()`
   (`src/guest_signal.c:282`). Tests are in Tasks 4 and 8.
5. **Process state across fork and exec.** `PR_SET_NO_NEW_PRIVS` survives
   both. `PR_SET_PDEATHSIG` is cleared in a fork child, as on Linux.
   `RLIMIT_NOFILE` keeps its Phase 1 exec carry. The test is in Task 10.

---

### Task 1: Node fix: `rt_sigaction` on signals with no QNX equivalent

**Files:**
- Modify: `src/guest_signal.c:512-550` (`guest_signal_action`) and
  `:456-510` (the send paths)
- Modify: `src/linux_syscall.c:284` (export the wrapper) and
  `src/linuxemu.h`
- Create: `guest-tests/signal-unmapped.S`
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-signal-smoke.sh` (`run_guest signal-unmapped 0`)

**Interfaces:**
- Produces: `int32_t linux_report_unsupported(uint32_t number,
  int32_t linux_error, const char *detail, const uint32_t *args);` in
  `linuxemu.h`. It is a non-static wrapper around `linux_unsupported`,
  used by every later task's new translation units.

- [ ] **Step 1: Set up.** Create the worktree and branch `phase2-syscalls`.
  Copy the device's `build/linuxemu` to `build/linuxemu-pre-phase2` and
  check its sha256 is `5089cc4c…`.
- [ ] **Step 2: Write the failing test `guest-tests/signal-unmapped.S`.**
  Each check is an exit code (all `rt_sigaction` calls use sigsetsize 8):
  - 10: `rt_sigaction(16, NULL, &old)` returns 0, and `old.handler == 0`.
  - 11: for every n in 1..64, `rt_sigaction(n, NULL, &old)` returns 0
    (the loop Node's `ResetSignalHandlers` runs).
  - 12: `rt_sigaction(50, &{handler=1 /*SIG_IGN*/}, NULL)` returns 0.
    A query then returns `handler == 1`.
  - 13: `rt_sigaction(9, &act, NULL)` and `(19, …)` return -EINVAL (-22).
  - 14: `rt_sigaction(0, …)` and `(65, …)` return -22.
  - 15: `kill(getpid(), 50)` returns -22. So does
    `tgkill(getpid(), gettid(), 16)`.
  - 16: a mapped signal is unchanged: install a SIGUSR1 (10) handler and
    send it to self; the handler runs.
- [ ] **Step 3: Run it on the pre-phase core.**
  Run `build/linuxemu-pre-phase2 build/guest-tests/signal-unmapped; echo $?`.
  Expected: 10.
- [ ] **Step 4: Implement.**
  - In `guest_signal_action`:
    - Validate 1 ≤ n ≤ 64 and sigsetsize 8.
    - Copy out the old action for every valid n.
    - Reject setting 9 and 19.
    - Store the action.
    - Call host `sigaction` only when `linux_signal_to_host(n) != 0`
      (and keep the existing 4/11 skip).
  - In the send paths (`guest_signal_send`, `guest_signal_send_thread`,
    `guest_signal_queue_thread`): an unmapped signal returns
    `linux_report_unsupported(nr, 22, "signal-unmapped", args)`. Signal 0
    (the existence probe) is unchanged.
- [ ] **Step 5: Run it on the new core.** Expected exit 0.
  `sh scripts/run-signal-smoke.sh` ends with 0 failures.
- [ ] **Step 6: Commit** with the message "Let guests query and set
  handlers for signals QNX lacks".

### Task 2: `cacheflush`, the JIT patch-hazard measurement, and Node's next blocker

**Files:**
- Modify: `src/linux_syscall.c` (add
  `case 0x000f0002u: result = linux_cacheflush(r0, r1, r2);` beside
  `LINUX_ARM_NR_SET_TLS`)
- Create: `guest-tests/cacheflush-syscalls.S`,
  `guest-tests/jit-patch-hazard.S`
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-linuxemu-smoke.sh` (`run_guest cacheflush-syscalls 0`)
- Modify: `docs/FINDINGS.md`, `docs/corpus-results.md`

**Interfaces:**
- Produces: `static int32_t linux_cacheflush(uint32_t start,
  uint32_t end, uint32_t flags)` (local to `linux_syscall.c`).

- [ ] **Step 1: Write the failing test `cacheflush-syscalls.S`.**
  - 10: mmap an RWX anonymous page. Write `mov r0,#42; bx lr`. Call
    `cacheflush(page, page+8, 0)`; it returns 0.
  - 11: calling the page returns 42.
  - 12: rewrite it to `mov r0,#43`, `cacheflush`, call; it returns 43.
    This proves the flush really happens, not merely that the call
    returns 0.
  - 13: `cacheflush(page+8, page, 0)` returns -22.
  - 14: `cacheflush(page, page+8, 1)` returns -22.
  - 15: a range that isn't guest memory (for example 0x1000–0x2000)
    returns -22.
- [ ] **Step 2: Run on the pre-phase core.** Expected exit 10 (ENOSYS).
- [ ] **Step 3: Implement `linux_cacheflush`.** Round to the page and
  require `guest_memory_runtime_owned()` or `guest_memory_range_owned()`,
  then call `msync(start, len, MS_INVALIDATE_ICACHE)`. Don't patch.
- [ ] **Step 4: Run on the new core.** Expected exit 0.
- [ ] **Step 5: Write the measurement test `jit-patch-hazard.S`.** It is
  not added to any regression runner.
  - Map RW anonymous memory and store a "literal pool" of 0xef000000 and
    0xee1d0f70 (`mrc p15,0,r0,c13,c0,3`) words.
  - `mprotect` it RX, then read the words back.
    - Exit 20: a word changed (the hazard is present).
    - Exit 0: the words are intact.
  - Then loop RW→RX `mprotect` 5000 times over one page that contains
    one `svc` word.
    - Exit 21: an `mprotect` failed (patch table full); print the
      iteration.
- [ ] **Step 6: Run the measurement** on the new core. Record both
  results (the exit code and the iteration) in `docs/FINDINGS.md` under
  "Phase 2: runtime-mapping patch hazard", with the Phase 4 hand-off from
  the spec.
- [ ] **Step 7: Rerun the Node corpus app.** Run `sh scripts/run-corpus.sh
  node` and read `build/corpus/node/` and its diagnostics report.
  - Expected: no `ResetSignalHandlers` abort.
  - Record the stage reached and the call it stops on (expected
    `epoll_create1`) in `docs/corpus-results.md` under a new "Phase 2"
    heading.
- [ ] **Step 8: Commit** with the message "Add ARM cacheflush and measure
  the runtime patch hazard".

### Task 3: `pipe2`, `dup3` and `accept4` edge coverage

**Files:**
- Create: `guest-tests/fd-flags-syscalls.S`, `guest-tests/dup3-race.c`
  and its fixture `guest-tests/fixtures/dup3-race-armhf`
- Modify: `src/linux_syscall.c` (only for defects the tests find; pipe2
  `O_DIRECT`)
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-process-smoke.sh` (`run_guest`-style entry for the `.S`
  test, `run_rootfs_fixture dup3-race`)

- [ ] **Step 1: Write `fd-flags-syscalls.S`.**
  - 10: `pipe2(fds, O_NONBLOCK|O_CLOEXEC = 0x80800)`. `F_GETFL` on both
    ends has 0x800, and `F_GETFD` on both is 1.
  - 11: `pipe2(fds, 0x1)` returns -22.
  - 12: `pipe2(fds, O_DIRECT = 0x10000)` returns -22, and with
    `LINUXEMU_DIAGNOSTICS` set the report has a `detail=pipe2-packet`
    line (the runner checks the report).
  - 13: `dup3(a, a, 0)` returns -22, and `dup3(a, b, 0x1)` returns -22.
  - 14: `dup3(a, b, O_CLOEXEC = 0x80000)` onto an open `b` returns `b`,
    `F_GETFD(b) == 1`, and `b` now reads `a`'s pipe.
  - 15: `dup3` onto a wake eventfd destination replaces it, and the old
    eventfd's hidden write end is released (`fd-close-all`-style check
    that the hidden count doesn't grow).
  - 16: `dup3(a, hidden_base, 0)` returns -9, and so does
    `dup3(hidden_base, a, 0)`. Read the base from `prlimit64`
    RLIMIT_NOFILE.
  - 17: `accept4(listener, NULL, NULL, SOCK_NONBLOCK|SOCK_CLOEXEC)` on a
    loopback connection gives an fd with 0x800 in its flags and
    `F_GETFD == 1`. `accept4(…, 0x1)` returns -22.
- [ ] **Step 2: Write `dup3-race.c`.**
  - Two threads, 20000 iterations each:
    - thread A: `dup3(src, 100, O_CLOEXEC)`;
    - thread B: `close(100)` and opens `/dev/null`.
  - Afterwards:
    - 10: no crash.
    - 11: `fd 100` is either closed (EBADF) or answers `F_GETFD`.
    - 12: a wake eventfd created before the race still reads and writes
      (its record wasn't clobbered).
  - Exit 0, or the check number.
- [ ] **Step 3: Run both on the pre-phase core** and record the result.
  - Expected: the `.S` test fails at 12, because the current flag check
    rejects 0x10000 with a plain -22 and no diagnostics entry. Run it
    with `LINUXEMU_DIAGNOSTICS` and check for the `pipe2-packet` line.
  - Checks that already pass on the old core are coverage, not fixes;
    list them in the commit message.
- [ ] **Step 4: Fix what fails.** Return -22 for pipe2 `O_DIRECT` through
  `linux_report_unsupported(359, 22, "pipe2-packet", args)`.
- [ ] **Step 5: Run on the new core.** Both exit 0;
  `run-process-smoke.sh` has 0 failures.
- [ ] **Step 6: Commit** with the message "Cover pipe2, dup3 and accept4
  flag and race cases".

### Task 4: Vector positional I/O and `sendfile64`

**Files:**
- Create: `src/linux_file_extra.c`
- Modify: `src/linuxemu.h`, `src/linux_syscall.c` (dispatch),
  `src/guest_signal.c:282` (restartable list)
- Create: `guest-tests/file-vector-syscalls.S`,
  `guest-tests/sendfile-interrupt.c` and its fixture
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-filesystem-smoke.sh`

**Interfaces:**
- Produces, in `linuxemu.h`:
  - `int32_t linux_preadv(int fd, const struct iovec *iov, int32_t count,
    int64_t offset, uint32_t flags, int vectored_kind);` where
    `vectored_kind` is 0 for `preadv` and 1 for `preadv2`.
  - `int32_t linux_pwritev(…same…);`
  - `int32_t linux_sendfile64(int out_fd, int in_fd, int64_t *offset,
    uint32_t count);`
- Consumes: `linux_report_unsupported` (Task 1).

- [ ] **Step 1: Write `file-vector-syscalls.S`.**
  The file is `/tmp/linuxemu-vec` with 64 bytes 'A'..; iovecs are 3×8
  bytes.
  - 10: `pwritev(fd, iov3, 3, 100, 0)` returns 24, and `lseek(fd,0,SEEK_CUR)`
    is unchanged (0).
  - 11: `preadv(fd, iov3, 3, 100, 0)` returns 24 with matching bytes.
  - 12: `preadv` at offset 120 of a 124-byte file returns 4 (a short
    read at EOF).
  - 13: `preadv2(fd, iov, 1, -1 (lo/hi = 0xffffffff), 0)` reads at the
    current position and advances it by 8.
  - 14: `pwritev2(…, flags = RWF_NOWAIT 8)` returns -95 (EOPNOTSUPP).
    So does `RWF_HIPRI 1`.
  - 15: `iovcnt = 1025` returns -22, and `iovcnt = -1` returns -22.
  - 16: offset hi = 0xffffffff (negative) returns -22.
  - 17: offset 5 GiB (lo = 0x40000000, hi = 1) on `preadv` returns 0
    (EOF), not an error from a truncated offset.
  - 18: a hidden-range fd returns -9.
  - 19: `sendfile64(out, in, &off=10, 20)` returns 20; `off == 30`; and
    the input fd's position is unchanged.
  - 20: `sendfile64(out, in, NULL, 20)` returns 20 and advances the
    input position by 20.
  - 21: `sendfile64` with a hidden in or out fd returns -9.
- [ ] **Step 2: Write `sendfile-interrupt.c`.**
  - A SIGALRM handler with `SA_RESTART` is installed, and `alarm`
    interrupts a blocking `sendfile64` from a file into a full pipe.
  - Expected: a positive partial count once bytes have moved, or a
    transparent restart before any have.
  - Then the same with `preadv` on an empty pipe, without `SA_RESTART`:
    it returns -4 (EINTR).
- [ ] **Step 3: Run on the pre-phase core.** Expected exit 10 (ENOSYS).
- [ ] **Step 4: Implement in `linux_file_extra.c`.**
  - The vector calls loop over `pread`/`pwrite` per iovec and stop at the
    first short transfer.
  - Once bytes have moved, an error returns the count.
  - Offset −1 on the `*2` calls goes to the existing `readv` /
    `linux_writev_compat` paths.
  - Flags: only 0 is accepted; `RWF_DSYNC` (2) and `RWF_SYNC` (4) are
    honoured by `fsync` after a successful write (the builder's call,
    taken here); others get EOPNOTSUPP with detail `rwf-flags`.
  - `sendfile64` loops through a 16 KiB stack buffer.
  - Add 239, 361, 362, 392 and 393 to `restartable_syscall`.
- [ ] **Step 5: Run on the new core.** Both tests exit 0, and
  `run-filesystem-smoke.sh` has 0 failures.
- [ ] **Step 6: Commit** with the message "Add vector positional I/O and
  sendfile64".

### Task 5: `fallocate` and the `fcntl64` additions

**Files:**
- Modify: `src/linux_file_extra.c`, `src/linux_syscall.c:693-772`
  (`linux_fcntl64`)
- Create: `guest-tests/fallocate-fcntl-syscalls.S`
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-filesystem-smoke.sh`

**Interfaces:**
- Produces: `int32_t linux_fallocate(int fd, uint32_t mode, int64_t offset,
  int64_t length);`. The arguments come from r0, r1, r2/r3 and r4/r5.

- [ ] **Step 1: Write the failing test.**
  - 10: `fallocate(fd, 0, 0, 8192)` returns 0, and `fstat` size ≥ 8192.
  - 11: `fallocate(fd, FALLOC_FL_KEEP_SIZE 1, …)` returns -95. So does
    `mode 3` (punch hole).
  - 12: a negative offset or a zero length returns -22.
  - 13: a hidden fd returns -9.
  - 14: `F_GETPIPE_SZ` (1032) on a pipe returns a positive value P.
    `F_SETPIPE_SZ` (1031) with P returns P, and with P×64 returns -1
    (EPERM).
  - 15: `F_SETOWN` (8) to `getpid()`, then `F_GETOWN` (9), returns
    `getpid()`. If QNX lacks owner support, both return -22 and the test
    accepts that pair consistently: decide from the device and record
    which in the commit message.
  - 16: `F_OFD_SETLK` (37) a write lock on bytes 0–9 returns 0.
    `F_OFD_GETLK` (36) from a fork child reports a write lock. Use the
    same struct as `F_GETLK64`; the `pid` field must be 0 in the request,
    else -22.
  - 17: `F_ADD_SEALS` (1033) and `F_GET_SEALS` (1034) return -22.
- [ ] **Step 2: Run on the pre-phase core.** Expected exit 10.
- [ ] **Step 3: Implement.**
  - `fallocate` mode 0 calls `posix_fallocate`, which returns an error
    number rather than setting errno; translate it with
    `linux_errno_number`.
  - Other modes get EOPNOTSUPP with detail `fallocate-mode`.
  - The new `fcntl64` commands go in `linux_fcntl64`'s switch. OFD locks
    reuse the 12/13/14 conversion, with the approximation noted in a
    comment.
- [ ] **Step 4: Run on the new core.** Expected exit 0, and the
  filesystem smoke stays green.
- [ ] **Step 5: Commit** with the message "Add fallocate and the
  remaining common fcntl64 commands".

### Task 6: `statx`, `statfs64` and an aligned `fstatfs64`

**Files:**
- Modify: `src/linux_syscall.c:451` (split `linux_stat_path` so it yields
  a host `struct stat`), `:607-634` (`linux_fstatfs64`)
- Modify: `src/linux_file_extra.c`
- Create: `guest-tests/statx-syscalls.S`
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-filesystem-smoke.sh`

**Interfaces:**
- Produces:
  - `int32_t linux_host_stat_at(int dirfd, const char *guest_path,
    uint32_t at_flags, struct stat *out);` handles `AT_FDCWD`,
    `AT_SYMLINK_NOFOLLOW` (0x100) and `AT_EMPTY_PATH` (0x1000). It
    returns 0 or a negative Linux errno. `linux_stat_path` is rebuilt on
    it.
  - `int32_t linux_statx(int dirfd, const char *path, uint32_t flags,
    uint32_t mask, void *out);`
  - `int32_t linux_statfs64_store(const struct statvfs *st,
    size_t guest_size, void *out);` is shared by `statfs64` and
    `fstatfs64`.

- [ ] **Step 1: Write the failing test.**
  - 10: `statx(AT_FDCWD, "/tmp", 0, 0x7ff, &buf)` returns 0. In `stx_mask`,
    `(mask & 0x7ff) == 0x7ff` and the btime bit 0x800 is clear.
    `stx_mode & S_IFMT` is a directory.
  - 11: on a regular file of 37 bytes, `stx_size == 37`, and
    `stx_ino`/`stx_nlink` equal `fstatat64`'s values.
  - 12: `statx(fd, "", AT_EMPTY_PATH, 0x7ff, &buf)` equals `fstat64(fd)`
    on ino and size.
  - 13: `AT_SYMLINK_NOFOLLOW` on a symlink gives `S_IFLNK`.
  - 14: flags 0x80000000 returns -22. `AT_STATX_SYNC_TYPE` values 0x2000
    and 0x4000 are accepted.
  - 15: `statx` with buffer NULL returns -14 (EFAULT).
  - 16: `AT_EMPTY_PATH` with a hidden fd returns -9.
  - 17: `statfs64("/tmp", 88, &b)` and `statfs64("/tmp", 84, &b)` both
    return 0; the 84 call doesn't write past byte 84 (guard word intact).
    Size 80 returns -22.
  - 18: `fstatfs64(fd, 84, &b)` returns 0 (today: -22).
- [ ] **Step 2: Run on the pre-phase core.** Expected exit 10.
- [ ] **Step 3: Implement.**
  - The `statx` struct is 256 bytes. Fill the `STATX_BASIC_STATS` fields
    from `struct stat`, set `stx_mask` to exactly those, and leave btime
    out.
  - `statfs64` resolves through the guest path translation, then calls
    `statvfs` and `linux_statfs64_store`.
- [ ] **Step 4: Run on the new core.** Expected exit 0.
  `run-filesystem-smoke.sh` and `run-python-smoke.sh` are green: Python
  now uses `statx` on every stat, so this is the wide check.
- [ ] **Step 5: Commit** with the message "Add statx and statfs64, and
  accept both statfs64 sizes".

### Task 7: `fchmod`, `faccessat2`, the `chown` family and `mknod`

**Files:**
- Modify: `src/linux_file_extra.c`, `src/linux_syscall.c` (dispatch)
- Create: `guest-tests/file-owner-syscalls.S`
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-filesystem-smoke.sh`

**Interfaces:**
- Produces:
  - `int32_t linux_chown_at(int dirfd, const char *path, uint32_t uid,
    uint32_t gid, uint32_t at_flags);` is used by all four chown calls.
    `fchown32` goes through `AT_EMPTY_PATH`.
  - `int32_t linux_mknod_at(int dirfd, const char *path, uint32_t mode,
    uint32_t dev);`

- [ ] **Step 1: Write the failing test.**
  - 10: `fchmod(fd, 0600)`, then `fstat` mode & 0777 == 0600.
    `fchmod(hidden)` returns -9.
  - 11: `faccessat2(AT_FDCWD, file, R_OK, 0)` returns 0.
    `faccessat2(…, AT_EACCESS 0x200)` returns 0. Flags 0x1 returns -22.
  - 12: `chown32(file, getuid(), getgid())` returns 0, and
    `chown32(file, -1, -1)` returns 0.
  - 13: `chown32(file, 0, 0)` returns -1 (EPERM) as the ordinary user.
  - 14: `lchown32` on a symlink and `fchownat(…, AT_SYMLINK_NOFOLLOW)`
    both return 0 for own ids.
  - 15: `mknod(path, S_IFIFO|0600, 0)` returns 0, and `stat` shows
    `S_IFIFO`.
  - 16: `mknod(path2, S_IFREG|0600, 0)` creates an empty file.
  - 17: `mknod(path3, S_IFCHR|0600, 0x0101)` returns -1 (EPERM), and so
    does `S_IFSOCK`.
  - 18: `mknodat(dirfd, "rel", S_IFIFO|0600, 0)` resolves against
    `dirfd`.
- [ ] **Step 2: Run on the pre-phase core.** Expected exit 10.
- [ ] **Step 3: Implement**, reusing `guest_path_resolve*` and the
  `*at` directory handling used by `linux_chmodat`.
- [ ] **Step 4: Run on the new core.** Expected exit 0, and the
  filesystem and package smokes are green (apk uses chown paths).
- [ ] **Step 5: Commit** with the message "Add fchmod, faccessat2, chown
  and mknod".

### Task 8: `sendmmsg` and `recvmmsg`

**Files:**
- Modify: `src/linux_socket.c` (the dispatch at `:811` and the socketcall
  table at `:807`)
- Modify: `src/guest_signal.c:282` (add 365 and 374)
- Create: `guest-tests/socket-mmsg-syscalls.S`
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-network-smoke.sh`

**Interfaces:**
- Consumes: the existing single-message `sendmsg`/`recvmsg` translation
  inside `linux_socket.c`. Factor it into
  `static int32_t socket_sendmsg_one(int fd, struct linux_msghdr *m,
  uint32_t flags)` and the matching `socket_recvmsg_one` if it isn't
  already separable.

- [ ] **Step 1: Write the failing test.** Use a UDP loopback pair and an
  AF_UNIX datagram socketpair.
  - 10: `sendmmsg(fd, 3 messages, 3, 0)` returns 3, and each
    `msg_len` equals its payload length.
  - 11: `recvmmsg(fd, 3 slots, 3, 0, NULL)` returns 3, with payloads in
    order.
  - 12: `recvmmsg(fd, slots, 3, MSG_DONTWAIT, NULL)` with 1 message
    queued returns 1.
  - 13: `recvmmsg(fd, slots, 3, MSG_WAITFORONE 0x10000, NULL)` with 2
    queued returns 2, without blocking for the third.
  - 14: the first message fails (the destination address on a
    connected-only socket is invalid) → the error; a later one fails →
    the count so far.
  - 15: `SCM_RIGHTS` in message 2 of a `sendmmsg` over the unix pair
    arrives as a working fd in `recvmmsg` slot 2.
  - 16: `vlen 2000` is clamped: no error, returns ≤ 1024.
  - 17: a hidden fd returns -9.
  - 18: a signal arrives while `recvmmsg` blocks with no data and
    `SA_RESTART` is set: it restarts, then returns when data is sent from
    a child.
- [ ] **Step 2: Run on the pre-phase core.** Expected exit 10.
- [ ] **Step 3: Implement** both calls as loops over the per-message
  helpers. Clamp `vlen` to 1024. Add the socketcall slots if the table
  covers 19/20.
- [ ] **Step 4: Run on the new core.** Expected exit 0,
  `run-network-smoke.sh` is green, and the `unix-rights-backlog` test
  still passes.
- [ ] **Step 5: Commit** with the message "Add sendmmsg and recvmmsg".

### Task 9: Resource limits, `getrusage`, `sysinfo`, `setitimer`/`getitimer`

**Files:**
- Create: `src/linux_resource.c`
- Modify: `src/linux_syscall.c` (move `linux_prlimit64` and
  `linux_resource_number` into `linux_resource.c`, then dispatch);
  `src/linuxemu.h`
- Create: `guest-tests/resource-syscalls.S`
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-linuxemu-smoke.sh`

**Interfaces:**
- Produces, in `linuxemu.h`:
  - `int32_t linux_resource_syscall(uint32_t number, uint32_t args[6]);`
    handles 75, 77, 104, 105, 116, 191 and 369 (prlimit64), so dispatch
    is one line per number.

- [ ] **Step 1: Write the failing test.**
  - 10: `ugetrlimit(RLIMIT_NOFILE 7, &r32)` matches `prlimit64`'s
    virtual value (936 on the Passport).
  - 11: `setrlimit(7, {100, 936})`, then `ugetrlimit`, gives cur 100.
  - 12: `ugetrlimit(RLIMIT_STACK 3)` returns 0. Raising cur above max
    gives -22, and raising max gives -1 (EPERM).
  - 13: an infinite host value reads as 0xffffffff in the 32-bit struct.
  - 14: `getrusage(RUSAGE_SELF 0, &ru)` returns 0, and
    `ru_utime + ru_stime > 0` after a busy loop.
    `getrusage(RUSAGE_CHILDREN -1)` returns 0. `getrusage(RUSAGE_THREAD
    1)` returns 0 with thread time > 0. `who = 5` returns -22.
  - 15: `getrusage(0, NULL)` returns -14.
  - 16: `sysinfo(&si)` returns 0, `uptime > 0`, `totalram * mem_unit`
    > 64 MiB, `freeram <= totalram`, `procs ≥ 1`.
  - 17: `sysinfo(NULL)` returns -14.
  - 18: `setitimer(ITIMER_REAL, {0, 50000 µs})` with a SIGALRM handler:
    the handler runs within 1 s. `getitimer` before expiry shows a
    remaining time > 0.
  - 19: `setitimer(ITIMER_VIRTUAL 1, …)` returns 0 or -22, consistently
    with getitimer. Record which on the device. `which = 3` returns -22.
  - 20: `getitimer(0, NULL)` returns -14.
- [ ] **Step 2: Run on the pre-phase core.** Expected exit 10.
- [ ] **Step 3: Implement.**
  - `getrusage` reuses `linux_rusage_store` (`src/linux_abi.c:183`).
    `RUSAGE_THREAD` uses the thread CPU clock.
  - `sysinfo`'s struct is 64 bytes. For memory, read QNX's totals (try
    `sysconf(_SC_PHYS_PAGES)`/`_SC_AVPHYS_PAGES` first, then `/proc`) and
    record which source in a code comment. Loads are 0.
  - Itimers use QNX `setitimer`/`getitimer` with 32-bit timevals.
- [ ] **Step 4: Run on the new core.** Expected exit 0, and the linuxemu
  and signal smokes are green.
- [ ] **Step 5: Commit** with the message "Add rlimit, getrusage, sysinfo
  and interval timers".

### Task 10: Priority, scheduler, affinity, capabilities, `prctl` and `membarrier`

**Files:**
- Modify: `src/linux_resource.c`, `src/linux_syscall.c:1308-1340` (move
  `linux_sched_getscheduler` and `linux_prctl` into
  `linux_resource.c`), `src/guest_process.c` (carry NO_NEW_PRIVS across
  exec as a hidden `LINUXEMU_GUEST_NO_NEW_PRIVS` variable, following the
  `LINUXEMU_GUEST_PERSONALITY` pattern), `src/guest_thread.c` (clear
  pdeathsig in the fork child)
- Create: `guest-tests/process-resource-syscalls.S`,
  `guest-tests/prctl-exec.c` and its fixture
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-linuxemu-smoke.sh`, `scripts/run-process-smoke.sh`

**Interfaces:**
- Extends `linux_resource_syscall` with 96, 97, 154, 155, 156, 172, 184,
  185, 241, 242 and 389, plus the existing `sched_getscheduler`.

- [ ] **Step 1: Write `process-resource-syscalls.S`.**
  - 10: `getpriority(PRIO_PROCESS 0, 0)` returns 20 − nice (20 at nice
    0). `setpriority(0, 0, 5)`, then `getpriority`, returns 15.
    `setpriority(0, 0, -5)` returns -13 (EACCES).
  - 11: `PRIO_PGRP 1` and `PRIO_USER 2` either work or return -22 with
    detail. Record which.
  - 12: `sched_getaffinity(0, 4, &m)` returns 4 and `m != 0`.
    `cpusetsize 2` returns -22. A NULL mask returns -14.
  - 13: `sched_setaffinity(0, 4, &one_cpu)`, then `getaffinity`, shows
    that one bit. A mask of 0 returns -22. pid 999999 returns -3
    (ESRCH).
  - 14: `sched_setscheduler(0, SCHED_OTHER 0, {0})` returns 0.
    `(0, SCHED_FIFO 1, {10})` returns -1 (EPERM). Policy 7 returns -22.
    `sched_getparam` gives priority 0.
  - 15: `capget({0x20080522, 0}, data[2])` returns 0 with every set
    zero. `capget({0x12345678, 0}, NULL)` returns -22 and writes
    0x20080522 back into the header.
  - 16: `capset` with all-zero sets returns 0. Setting
    `effective = 1 << 12` returns -1 (EPERM).
  - 17: `prctl(PR_GET_DUMPABLE 3)` returns 1. `PR_SET_DUMPABLE 4` with 0,
    then GET, returns 0. SET with 2 returns -22.
  - 18: `prctl(PR_SET_NO_NEW_PRIVS 38, 1, 0, 0, 0)` returns 0, then GET
    (39) returns 1. `PR_SET_NO_NEW_PRIVS` with 0 returns -22.
  - 19: `prctl(PR_SET_PDEATHSIG 1, SIGTERM)`, then
    `PR_GET_PDEATHSIG 2, &v`, gives v == 15. Signal 65 returns -22.
  - 20: `membarrier(MEMBARRIER_CMD_QUERY 0, 0, 0)` returns 0.
    `membarrier(8 /*PRIVATE_EXPEDITED*/, 0, 0)` returns -22.
- [ ] **Step 2: Write `prctl-exec.c`** (Review Focus 5).
  - Set NO_NEW_PRIVS and PDEATHSIG, then fork.
  - The child checks NO_NEW_PRIVS == 1 and PDEATHSIG == 0.
  - Exec self with an argument. The exec'd image checks NO_NEW_PRIVS
    == 1.
  - Exit 0, or a check number.
- [ ] **Step 3: Run both on the pre-phase core.** Expected exit 10.
- [ ] **Step 4: Implement.**
  - Priority conversion: Linux nice ↔ QNX. Check QNX's `getpriority`
    return convention on the device first.
  - Affinity uses `ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET)` for the
    calling thread only. Another existing task gets EPERM with detail
    `affinity-other`.
  - Capabilities: versions 1, 2 and 3 have 1, 2 and 2 data elements.
  - PDEATHSIG is stored, and delivery is reported unsupported in the
    diagnostics by detail `pdeathsig-delivery` when it's set to non-zero.
    First check whether QNX offers a parent-death notification. If it
    does, use it and drop the detail; record what you found in the commit
    message.
- [ ] **Step 5: Run on the new core.** Both tests exit 0, and the
  linuxemu and process smokes are green.
- [ ] **Step 6: Commit** with the message "Add priority, affinity,
  scheduler, capability, prctl and membarrier calls".

### Task 11: `mremap`

**Files:**
- Modify: `src/linux_syscall.c` (beside `linux_munmap_runtime` at
  `:1249`), `src/guest_memory.c` (a query for whether a range is one
  anonymous, non-executable runtime mapping, and for its extent)
- Create: `guest-tests/mremap-syscalls.S`
- Modify: `scripts/build-native-probes.sh`,
  `scripts/run-linuxemu-smoke.sh`

**Interfaces:**
- Produces: `int guest_memory_runtime_is_plain(uintptr_t address,
  size_t length);` (anonymous, non-executable, wholly tracked) and
  `static int32_t linux_mremap(uintptr_t old, size_t old_len,
  size_t new_len, uint32_t flags, uintptr_t new_addr)`.

- [ ] **Step 1: Write the failing test.**
  - 10: mmap 3 pages RW anonymous, fill them with a pattern, then
    `mremap(a, 3p, 1p, 0)` returns `a`. Page 1 is still readable and
    keeps its pattern; the freed pages are released (a fresh `mmap` hint
    may reuse them).
  - 11: `mremap(a, 1p, 4p, MREMAP_MAYMOVE 1)` returns `b`, `b[0..1p]`
    keeps the pattern, and the new pages are zero.
  - 12: growing without MAYMOVE when the next range is occupied returns
    -12 (ENOMEM).
  - 13: an RX mapping returns -22. So do flags `MREMAP_FIXED 2` and
    `MREMAP_DONTUNMAP 4`.
  - 14: a file-backed mapping returns -22.
  - 15: an unaligned old address returns -22.
- [ ] **Step 2: Run on the pre-phase core.** Expected exit 10.
- [ ] **Step 3: Implement.**
  - Shrink: `munmap` the tail and update the registry.
  - Grow in place: only if `guest_memory` says the following range is
    free and a fixed `mmap` there succeeds.
  - Move: `mmap` new, `memcpy`, then the old range goes through
    `linux_munmap_runtime`.
  - Unsupported cases get EINVAL with detail `mremap-kind`.
- [ ] **Step 4: Run on the new core.** Expected exit 0.
  `run-loader-process-stress.sh` and `run-dynamic-smoke.sh` are green
  (musl `realloc` now takes this path).
- [ ] **Step 5: Commit** with the message "Add mremap for plain anonymous
  guest memory".

### Task 12: Phase gate and docs

**Files:**
- Modify: `docs/corpus-results.md`, `docs/compatibility.md`,
  `docs/development.md` (verified behaviour and detailed limits),
  `docs/FINDINGS.md`

- [ ] **Step 1: Run the full regression.** Run
  `ssh -tt -o HostName=<passport-ip> bb10 'cd DEV && sh scripts/run-full-regression.sh'`.
  Expected: `linuxemu_full_regression_failures=0` in
  `build/full-regression/summary.txt`.
- [ ] **Step 2: Rerun the corpus.** Run `sh scripts/run-corpus.sh`.
  Expected:
  - at least 7 of 11 compatible;
  - none of the calls this phase implemented appear in any app's
    unsupported list;
  - Node is past `ResetSignalHandlers`.
- [ ] **Step 3: Update the docs.**
  - `docs/corpus-results.md` gets a "Phase 2" baseline section: the new
    stage table and unsupported-call table, plus Node's next blocker.
  - `docs/compatibility.md` and `docs/development.md` list the new calls
    and the spec's known limits:
    - vector I/O is not atomic across iovecs;
    - OFD locks are per-process;
    - unmapped signals can't be sent;
    - `membarrier` supports no commands;
    - `mremap` handles plain anonymous memory only;
    - the patch hazard.
  - Leave `docs/capability-roadmap.md` alone: it holds the user's
    uncommitted edits. Tell the user its Phase 2 status line needs
    updating.
- [ ] **Step 4: Commit** with the message "Record Phase 2 results". Then
  hand off to superpowers:finishing-a-development-branch (final review,
  fast-forward merge into master; push only when the user asks).
