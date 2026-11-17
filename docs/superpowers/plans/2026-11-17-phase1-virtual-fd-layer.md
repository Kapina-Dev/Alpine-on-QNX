# Phase 1: Virtual File-Descriptor Layer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace Linuxemu's fixed per-fd arrays with one locked,
reference-counted record layer. Then fix the descriptor defects it exposes:
- `dup` of the eventfd;
- fds ≥ 256;
- records lost across `exec`;
- stale records;
- races;
- a visible eventfd write end.

**Architecture:**
- Guest fd numbers stay native QNX numbers (approach B).
- A new module, `src/guest_fd.c`, maps an fd number to an optional shared,
  reference-counted object. No object means a plain native fd.
- Objects are identified by their backing native fd's `(st_dev, st_ino)`.
- Syscall code calls the module instead of owning arrays.

**Tech Stack:**
- C (gnu99), BB10 SDK gcc 4.6.3, building on the Passport.
- ARM assembly guest tests and C guest fixtures.
- A host-side unit test, built with the laptop's `cc`.

**Spec:** `docs/superpowers/specs/2026-11-17-phase1-virtual-fd-layer-design.md`
(roadmap: `docs/capability-roadmap.md`, "Phase 1").

## Global Constraints

- Approach B: guest fd = native fd. No decoupled numbering.
- **Every fixed defect gets a guest test that fails before the fix and
  passes after it.** Watch it fail on the device.
- Builds must be warning-free under
  `-Wall -Wextra -O2 -std=gnu99 -marm` (device SDK gcc).
  `src/guest_fd.c` must also build on the Linux host under
  `cc -Wall -Wextra -pthread` for its unit test.
- **The hot path for native fds:** `read`, `write` and `poll` may add at
  most one lock-free check, with no syscall and no lock.
- **Shared state:** state shared across `fork` must live in a kernel object
  (a pipe). Process memory may only cache it.
- Don't touch the roadmap phases beyond Phase 1:
  - full eventfd semantics stay Phase 2 (keep the wake subset:
    `initval 0`, flags exactly `EFD_NONBLOCK|EFD_CLOEXEC`, writes of the
    value 1 only);
  - epoll, timerfd, signalfd and inotify stay Phase 3 (type enumerators
    only).
- **The user's uncommitted edits stay out of commits.**
  - Leave the two `runtime_trace_enabled()` blocks and
    `#include <stdio.h>` in `src/linux_socket.c` out of every commit. To
    commit that file, stage a copy without them (`git hash-object -w` +
    `git update-index --cacheinfo 100644,<blob>,src/linux_socket.c`),
    compile-check that copy on the device, and leave the working tree
    untouched.
  - Never stage `docs/capability-roadmap.md`, `docs/compatibility.md`,
    `docs/release-notes-v0.1.6.md`,
    `root-analysis/native-probe-results-20260922.md`, `ANDROID6_PLAN.md`,
    `native-probes/unix-rights-pressure.c` or
    `root-analysis/terminal-debug-20261014/`.
- Commit messages end with the session's attribution lines.

**Device commands** (dev checkout `D=/accounts/1000/shared/misc/linuxemu-dev`,
`ssh -o HostName=192.168.0.3 bb10`; copy changed files with `scp` first):

- **Core build:**
  ```sh
  cd $D && export QNX_HOST=$PWD/sdk QNX_TARGET=$PWD/sdk/target_10_3_1_995/qnx6 LD_LIBRARY_PATH=$PWD/sdk/lib PATH=$PWD/sdk/bin:$PATH && sdk/bin/gcc --sysroot=$QNX_TARGET -Wall -Wextra -O2 -std=gnu99 -marm -Isrc -o build/linuxemu src/*.c -lsocket
  ```
  Expected: no output.
- **Assembly guest test `<t>`:**
  ```sh
  LD_LIBRARY_PATH=$PWD/sdk/lib sdk/bin/as -o build/guest-tests/<t>.o guest-tests/<t>.S && LD_LIBRARY_PATH=$PWD/sdk/lib sdk/bin/ld -T guest-tests/minimal-arm.ld -o build/guest-tests/<t> build/guest-tests/<t>.o
  ```
  Run it with `build/linuxemu build/guest-tests/<t>; echo exit=$?`.
- **Host unit test** (laptop):
  ```sh
  cc -Isrc -Wall -Wextra -pthread -o build/guest-fd-test tests/guest-fd-test.c src/guest_fd.c && build/guest-fd-test
  ```
  Expected: `guest_fd_test_failures=0`.
- **Full regression:**
  ```sh
  ssh -tt ... 'cd $D && sh scripts/run-full-regression.sh > /dev/tty 2>&1'
  ```
  Then read `$D/build/full-regression/summary.txt`. Expected:
  `linuxemu_full_regression_failures=0`. If the only failure is
  `run-python-smoke` with the known `pc=0x60067dc4` fault, that is the
  pre-existing state and is accepted.

## Review Focus

- **A guest closes every fd** (the daemon or `subprocess` close-all loop
  from 3 up to `RLIMIT_NOFILE`). Emulator objects must survive. Pinned by
  the Task 6 test `fd-close-all`.
- **A guest replaces a typed fd with `dup2`/`dup3`** (`dup2(pipe_fd,
  eventfd_number)`). The old object loses that reference, and the new
  number behaves as a pipe. Pinned in the Task 3 test `wake-eventfd-dup`.
- **`exec` with a close-on-exec typed fd.** Its record and hidden fds must
  not carry over, and no hidden fd may leak into the new image. Pinned in
  the Task 5 fixture `fd-exec-carry` (the leak count after exec).
- **A malformed or hostile `LINUXEMU_FD_RECORDS`** (truncated, a
  non-number, a non-hex path, a huge count). Start-up must ignore the bad
  record and continue, not crash or attach garbage. Pinned in the Task 5
  host unit test `exec_records_reject_malformed`.
- **Threads racing `close` against `dup`** on the same typed fd. There must
  be no double release and no use after free. Pinned by the Task 7 fixture
  `fd-stress` (the threaded phase).

---

### Task 1: Record table core

**Files:**
- Create: `src/guest_fd.h`, `src/guest_fd.c`
- Create: `tests/guest-fd-test.c`
- Modify: `src/guest_thread.c` (the after-fork hook, next to
  `linux_futex_after_fork()`)
- Modify: `src/linuxemu.c` (call `guest_fd_initialize()` after
  `guest_thread_initialize()`)

**Interfaces:**
- Produces (`src/guest_fd.h`):
  ```c
  enum guest_fd_type { GUEST_FD_PATH = 1, GUEST_FD_WAKE_EVENTFD,
      GUEST_FD_EVENTFD, GUEST_FD_EPOLL, GUEST_FD_TIMERFD, GUEST_FD_SIGNALFD,
      GUEST_FD_INOTIFY, GUEST_FD_ASHMEM, GUEST_FD_BINDER };
  struct guest_fd_object;
  struct guest_fd_ops {
      int32_t (*read)(struct guest_fd_object *, int fd, void *, size_t);
      int32_t (*write)(struct guest_fd_object *, int fd, const void *, size_t);
      int (*poll_fd)(struct guest_fd_object *, int fd);
      int (*get_status)(struct guest_fd_object *, int fd);
      int (*set_status)(struct guest_fd_object *, int fd, int host_flags);
      void (*release)(struct guest_fd_object *);
      int (*serialize)(struct guest_fd_object *, int fd, char *, size_t);
  };
  /* Created with one reference owned by the caller; identity is taken
     from fstat(backing_fd).  Returns 0 on failure (errno set). */
  struct guest_fd_object *guest_fd_object_new(enum guest_fd_type type,
      const struct guest_fd_ops *ops, int backing_fd, size_t extra_size);
  void *guest_fd_object_data(struct guest_fd_object *);   /* extra_size bytes */
  enum guest_fd_type guest_fd_object_type(const struct guest_fd_object *);
  void guest_fd_put(struct guest_fd_object *);            /* drop one reference */
  int guest_fd_attach(int fd, struct guest_fd_object *);  /* slot takes its own reference; replaces any old one */
  void guest_fd_clear(int fd);                            /* number now native */
  int guest_fd_duplicate(int source, int destination);    /* after a native dup: destination shares source's object or becomes native */
  struct guest_fd_object *guest_fd_acquire(int fd);       /* referenced object or 0; validates identity, drops a mismatched record */
  int guest_fd_is_typed(int fd);                          /* lock-free hint */
  int guest_fd_initialize(void);
  void guest_fd_after_fork(void);
  int guest_fd_hidden_base(void);
  int guest_fd_hide(int fd);  /* moves an emulator-owned fd to >= hidden base, close-on-exec; returns new number, closes old */
  ```
- **Hidden base:**
  - `guest_fd_initialize()` raises the host soft `RLIMIT_NOFILE` to the hard
    limit (best effort), then sets `base = soft - 64`.
  - Record the measured soft/hard values in the commit message.
  - If the limit is unbounded or above 4096, use `base = 4032`.

- [ ] **Step 1: Write the failing host unit test `tests/guest-fd-test.c`**

  Use real pipes as backing fds. The cases:
  - `attach_then_acquire`: an attached object is returned by `acquire` with
    the same type. `guest_fd_is_typed(fd)` is 1. After `clear`, `acquire`
    returns 0 and `is_typed` is 0.
  - `duplicate_shares_object`: after `dup(fd)` and
    `guest_fd_duplicate(fd, d)`, both numbers return the same object.
    Closing and clearing one leaves the other attached. The `release` op
    runs exactly once, after the last clear.
  - `identity_mismatch_drops_record`: attach to fd A (pipe 1), then natively
    `dup2(pipe2_read, A)` behind the table's back. `acquire(A)` returns 0,
    and `release` has run.
  - `fd_above_256`: attach on fd `dup2(x, 300)` works.
  - `hide_moves_above_base`: the returned fd is `>= guest_fd_hidden_base()`,
    close-on-exec is set, and the original number is closed.

  Print `guest_fd_test_failures=<n>` and exit with `n != 0`.

- [ ] **Step 2: Run the host unit test.** Expected: a compile failure,
  because `guest_fd.h` doesn't exist yet.
- [ ] **Step 3: Implement `src/guest_fd.c`.**
  - A growable `struct guest_fd_object **` array indexed by fd.
  - A typed-bitmap or counter for `guest_fd_is_typed`, readable without the
    lock.
  - One `pthread_mutex_t` protecting the slots and reference counts.
  - `guest_fd_after_fork()` re-initialises the mutex.
- [ ] **Step 4: Run the host unit test.** Expected:
  `guest_fd_test_failures=0`.
- [ ] **Step 5: Wire it in.** Call `guest_fd_initialize()` from
  `src/linuxemu.c` (print `perror("linuxemu fd table")` and return 1 on
  failure), and `guest_fd_after_fork()` from the `guest_thread.c` after-fork
  path. Then do the device core build: no warnings. Run the full
  regression: unchanged pass state.
- [ ] **Step 6: Commit**
  `git add src/guest_fd.h src/guest_fd.c tests/guest-fd-test.c src/guest_thread.c src/linuxemu.c`.
  Message: "Add the guest fd record table core".

### Task 2: Path records move onto the table

**Files:**
- Modify: `src/guest_fd.c`, `src/guest_fd.h`
- Modify: `src/linux_syscall.c`. Remove `fd_paths` and `directory_streams`.
  Rewrite `directory_path`, `linux_open_path`, `linux_close_fd`,
  `copy_fd_path` (delete it; use `guest_fd_duplicate`), the `getdents64`
  path and the `FCHDIR` case.
- Create: `guest-tests/fd-path-records.S`
- Modify: `scripts/build-native-probes.sh` (guest list),
  `scripts/run-filesystem-smoke.sh`

**Interfaces:**
- Consumes: Task 1 API.
- Produces:
  ```c
  int guest_fd_record_path(int fd, const char *host_path);   /* attach a GUEST_FD_PATH object; 0 or -1 */
  int guest_fd_path(int fd, char *host_path, size_t size);   /* 0, or -1 with errno EBADF if no path record */
  DIR *guest_fd_directory_stream(int fd);                    /* lazily opendir()s the path; stream owned by the shared object; 0 + errno on failure */
  ```

- [ ] **Step 1: Write the failing guest test
  `guest-tests/fd-path-records.S`** (exit codes 130 and up).
  - `openat(AT_FDCWD, "/", O_RDONLY|O_DIRECTORY)` → d.
  - `dup2(d, 300)`.
  - `getdents64(300, buf, 4096)` > 0 (exit 131 otherwise).
  - `fstatat64(300, "etc", ...)` == 0 (132).
  - `fchdir(300)` == 0 (133).
  - `close(d)`, then `getdents64(300, ...)` still > 0 after
    `lseek(300, 0, SEEK_SET)` (134).
  - Exit 0.

  Add it to the guest list in `build-native-probes.sh`. Add a block to
  `run-filesystem-smoke.sh` in the same form as the `unix-rights-backlog`
  block in `scripts/run-network-smoke.sh` (`expected=0`).
- [ ] **Step 2: Assemble and run it on the device with the current core.**
  Expected: `exit=131` (an fd ≥ 256 has no path).
- [ ] **Step 3: Implement the three functions, and replace every
  `fd_paths`/`directory_streams` use in `linux_syscall.c`.**
  - `/dev/ptmx` and plain opens record their path.
  - The `dup` family and `F_DUPFD*` call `guest_fd_duplicate`.
  - `close` calls `guest_fd_clear` before the native close.
- [ ] **Step 4: Rebuild the core and rerun the test.** Expected: `exit=0`.
- [ ] **Step 5: Run the full regression.** Expected: unchanged pass state.
- [ ] **Step 6: Commit** the source, test, scripts and `tests/guest-fd-test.c`
  (add a `path_record_shared_across_dup` unit case). Message: "Keep fd path
  records in the record table".

### Task 3: Wake eventfd on the table (dup support, hidden write end, status flags)

**Files:**
- Modify: `src/linux_syscall.c`. Remove `eventfd_write_peers`,
  `is_wake_eventfd` and `release_wake_eventfd`. Rewrite `linux_eventfd2`, the
  `READ`/`WRITE` cases, `DUP`/`DUP2`/`DUP3`, and `linux_fcntl64` (drop the
  wake-eventfd EOPNOTSUPP branches).
- Modify: `src/guest_fd.c`, `src/guest_fd.h`
- Create: `guest-tests/wake-eventfd-dup.S`
- Modify: `scripts/build-native-probes.sh`, `scripts/run-filesystem-smoke.sh`

**Interfaces:**
- Consumes: Task 1 (`guest_fd_object_new`, `guest_fd_hide`,
  `guest_fd_attach`, `guest_fd_duplicate`).
- Produces:
  ```c
  /* 1 if fd is typed and the op handled it (result set), 0 if native. */
  int guest_fd_dispatch_read(int fd, void *buffer, size_t length, int32_t *result);
  int guest_fd_dispatch_write(int fd, const void *buffer, size_t length, int32_t *result);
  int guest_fd_dispatch_get_status(int fd, int *host_flags);
  int guest_fd_dispatch_set_status(int fd, int host_flags);
  ```
- The wake eventfd object's data holds the hidden write-end fd.
  - `release` closes it.
  - `poll_fd` returns the guest number itself.
  - `set_status` applies `O_NONBLOCK` to both pipe ends.
  - Read and write keep today's subset semantics.

- [ ] **Step 1: Write the failing guest test
  `guest-tests/wake-eventfd-dup.S`** (exit codes 140 and up).
  - `eventfd2(0, EFD_NONBLOCK|EFD_CLOEXEC)` → e.
  - `dup(0)` → n. Require `n == e + 1` (141: the write end must not use a
    low number). Then `close(n)`.
  - `dup(e)` → d (142 if negative).
  - Write the 8-byte value 1 to d → 8 (143).
  - Read 8 bytes from e → 8 (144).
  - `close(e)`. Write 1 to d → 8 (145). Read from d → 8 (146).
  - **Review Focus "dup2 replacement":** `pipe2(p)`, then `dup2(p[0], d)`.
    Writing 8 bytes to `p[1]` and reading 8 from d returns 8 (147). A
    second read from d returns -EAGAIN, not eventfd data (148).
  - Exit 0.

  Add it to both scripts.
- [ ] **Step 2: Run it on the current core.** Expected: `exit=141`.
- [ ] **Step 3: Implement the wake eventfd object.**
  - Create the pipe and hide the write end with `guest_fd_hide`.
  - Attach the object to the read end.
  - Route `READ`/`WRITE` through `guest_fd_dispatch_*`, behind
    `guest_fd_is_typed`.
  - Route `F_GETFL`/`F_SETFL` through `guest_fd_dispatch_get_status`/`_set_status`
    when typed.
- [ ] **Step 4: Rebuild and rerun the test.** Expected: `exit=0`. Also rerun
  `run-network-smoke.sh` (libcurl's eventfd use) and expect
  `linuxemu_network_smoke_failures=0`.
- [ ] **Step 5: Run the full regression.** Expected: unchanged pass state.
- [ ] **Step 6: Commit.** Message: "Move the wake eventfd onto the record
  table".

### Task 4: Every fd-creating path leaves a clean record

**Files:**
- Modify: `src/linux_socket.c`:
  - `make_socket`, `socket_pair` and `accept_socket` call
    `guest_fd_clear` on each new fd;
  - `receive_control` calls `guest_fd_received` on each received fd
    (stage around the user's trace lines; see Global Constraints).
- Modify: `src/linux_syscall.c`: `linux_pipe` / `pipe2` and any other native
  creation site call `guest_fd_clear`. Grep for `open(`, `socket(`,
  `pipe(`, `accept(` and `dup(`.
- Modify: `src/guest_fd.c`, `src/guest_fd.h`, `tests/guest-fd-test.c`

**Interfaces:**
- Produces: `void guest_fd_received(int fd);`. In this task it equals
  `guest_fd_clear`. Task 8 adds adoption to it.

- [ ] **Step 1: Write the failing unit case `stale_slot_cleared_on_create`.**
  - Attach an object to fd N.
  - Natively close N without clearing it (simulating a missed path).
  - Create a new pipe that lands on N, and call `guest_fd_received(N)`.
  - `acquire(N)` must return 0 and `release` must have run once.
- [ ] **Step 2: Run the host unit test.** Expected: compile failure
  (`guest_fd_received` undefined).
- [ ] **Step 3: Implement `guest_fd_received`, and add the clear calls at
  every creation site listed above.**
- [ ] **Step 4: Run the host unit test.** Expected: 0 failures. Then do the
  device core build: no warnings.
- [ ] **Step 5: Run the full regression.** Expected: unchanged pass state.
- [ ] **Step 6: Commit.** For `linux_socket.c`, use the staged copy without
  the user's trace lines, compile-checked on the device. Message: "Clear fd
  records on every descriptor-creating path".

### Task 5: Exec carry-over

**Files:**
- Modify: `src/guest_fd.c`, `src/guest_fd.h`
- Modify: `src/guest_process.c`:
  - `hidden_environment()` also hides `LINUXEMU_FD_RECORDS=`;
  - `guest_process_exec` appends the records variable;
  - just before `execve`, clear close-on-exec on the hidden fds of carried
    objects.
- Create: `guest-tests/fd-exec-carry.c`, `scripts/build-guest-fixture.sh`,
  `guest-tests/fixtures/fd-exec-carry-armhf`
- Modify: `scripts/run-process-smoke.sh`, `tests/guest-fd-test.c`

**Interfaces:**
- Produces:
  ```c
  /* Builds "LINUXEMU_FD_RECORDS=..." for fds that survive exec (no FD_CLOEXEC) and carry a typed record; returns length, 0 if none, -1 if it doesn't fit. */
  int guest_fd_exec_records(char *buffer, size_t size);
  int guest_fd_exec_prepare(void);   /* clear FD_CLOEXEC on hidden fds of carried objects */
  int guest_fd_restore_records(const char *value);   /* parse at start-up; skips malformed records; returns count restored */
  ```
  `guest_fd_initialize()` calls `guest_fd_restore_records(getenv("LINUXEMU_FD_RECORDS"))`,
  then unsets the variable.
- **Format (my call):** `fd:type:hidden_fd:hexpath;` for each record.
  - `hidden_fd` is `-1` when there is none.
  - `hexpath` is empty for a non-path record.
  - Restored hidden fds get close-on-exec set again.
  - A record whose fd or hidden fd is not open, or whose identity doesn't
    match a freshly created object, is skipped.

- [ ] **Step 1: Write the failing tests.**
  - **Host unit `exec_records_round_trip`:** a path record and a wake
    eventfd record go through `guest_fd_exec_records`, then
    `guest_fd_clear` both, then `guest_fd_restore_records`. Both are
    restored with the same types.
  - **Host unit `exec_records_reject_malformed`:** `"5:2:x:;"`,
    `"5:99:-1:;"`, `"5:1:-1:zz;"`, a 100000-character string and `"7"`
    each restore 0 records without crashing.
  - **C fixture `guest-tests/fd-exec-carry.c`**, in two modes selected by
    `argv[1]`:
    - The parent opens `/etc` and `dup2`s it to 300, creates a wake
      eventfd e (`EFD_NONBLOCK` only, so not close-on-exec) and a second
      one, ec, with `EFD_CLOEXEC`, then counts its open fds. It execs
      itself as `child 300 <e> <count>`.
    - The child checks:
      - `getdents64(300)` > 0 (exit 2);
      - writing the value 1 to e and reading it back succeed (exit 3);
      - the number of open fds (scanning `F_GETFD` from 0 up to the
        `RLIMIT_NOFILE` soft limit) equals the parent's count minus 1, for
        ec (exit 4).
    - Exit 0.
  - Create `scripts/build-guest-fixture.sh NAME`: a generalised copy of
    `scripts/build-pthread-fixture.sh` that builds
    `guest-tests/NAME.c` into `guest-tests/fixtures/NAME-armhf`. Use an
    `ALPINE_ROOT` with `build-base` installed, for example a corpus
    gcc-make copy. Add a `run-process-smoke.sh` block (`expected=0`).
- [ ] **Step 2: Run the unit tests and the fixture on the current core.**
  Expected: unit compile failure; fixture `exit=2`.
- [ ] **Step 3: Implement the three functions and the `guest_process.c`
  changes.**
- [ ] **Step 4: Rerun the tests.** Expected: unit 0 failures, fixture
  `exit=0`.
- [ ] **Step 5: Run the full regression.** Expected: unchanged pass state.
- [ ] **Step 6: Commit** the source, tests, fixture binary and scripts.
  Message: "Carry fd records across exec".

### Task 6: Hidden range guards, virtual RLIMIT_NOFILE, poll translation

**Files:**
- Modify: `src/linux_syscall.c`:
  - `linux_prlimit64` virtualises `RLIMIT_NOFILE`;
  - `close`, `DUP2`/`DUP3` targets and `F_DUPFD*` refuse the hidden range.
- Modify: `src/linux_poll.c`:
  - `do_poll` and `do_select` translate typed fds through `poll_fd`;
  - `POLLNVAL` for hidden numbers in poll, EBADF in select.
- Modify: `src/guest_fd.c`, `src/guest_fd.h`
- Create: `guest-tests/fd-close-all.S`
- Modify: `scripts/build-native-probes.sh`, `scripts/run-filesystem-smoke.sh`

**Interfaces:**
- Produces:
  ```c
  int guest_fd_is_hidden(int fd);                 /* fd >= hidden base */
  int guest_fd_poll_target(int fd);               /* native fd to poll for guest fd */
  void guest_fd_nofile_get(uint64_t *soft, uint64_t *hard);   /* virtual guest limit */
  int guest_fd_nofile_set(uint64_t soft, uint64_t hard);      /* clamps to hidden base; never touches the host limit */
  ```
- **Virtual limit:**
  - The initial value is `soft = hard = guest_fd_hidden_base()`.
  - Setting it above the base clamps to the base.
  - Lowering it is stored and reported, but not enforced.
  - The base is carried across `exec` as the first record of
    `LINUXEMU_FD_RECORDS`: `base:<n>;`.
- **Guard errors:**
  - `close` of a hidden number → EBADF;
  - a `dup2`/`dup3` target in the range → EBADF;
  - `F_DUPFD` with a minimum ≥ base → EINVAL.

- [ ] **Step 1: Write the failing guest test `guest-tests/fd-close-all.S`**
  (exit codes 150 and up).
  - `eventfd2(0, EFD_NONBLOCK)` → e.
  - `prlimit64(0, RLIMIT_NOFILE, 0, &old)` → cur.
  - For n from 3 to cur−1, skipping e, call `close(n)` and ignore the
    result.
  - Write the value 1 to e → 8 (151). Read → 8 (152).
  - `close(cur)` → -EBADF (153).
  - `dup2(0, cur)` → -EBADF (154).
  - `poll` on `{fd=cur}` sets `POLLNVAL` (155).
  - Exit 0.

  Add it to both scripts.
- [ ] **Step 2: Run it on the current core.** Expected: `exit=151`. The
  close-all loop closed the hidden write end, which sits below the host
  limit.
- [ ] **Step 3: Implement the guards, the virtual limit and the poll/select
  translation.**
- [ ] **Step 4: Rerun the test.** Expected: `exit=0`.
- [ ] **Step 5: Run the full regression.** Expected: unchanged pass state.
  Then run the corpus (`sh scripts/run-corpus.sh`, about 25 minutes).
  Expected: `corpus apps=11 compatible=7`, with the same failing stages as
  in `docs/corpus-results.md`.
- [ ] **Step 6: Commit.** Message: "Guard the hidden fd range and
  virtualise RLIMIT_NOFILE".

### Task 7: Stress gate: leaks and stale records

**Files:**
- Create: `guest-tests/fd-stress.c`, `guest-tests/fixtures/fd-stress-armhf`
- Modify: `scripts/run-process-smoke.sh`

**Interfaces:**
- Consumes: `scripts/build-guest-fixture.sh` (Task 5).

- [ ] **Step 1: Write the fixture `guest-tests/fd-stress.c`.** It counts
  open fds by scanning `F_GETFD` up to the soft limit, and takes the
  baseline at start. It has four phases:
  - **Phase A** (1000 iterations): open `/etc` as a directory fd, plus a
    wake eventfd and a pipe. Then `dup`, `dup2` onto 200 + i % 50,
    `dup3(..., O_CLOEXEC)`, and `F_DUPFD_CLOEXEC`. `getdents64` on one copy
    and an eventfd write/read on one copy must succeed (exit 2 and 3).
    Close everything.
  - **Phase B** (200 iterations): the same set, then `fork`. The child
    uses each object once and `_exit(0)`s. The parent reaps it with
    `waitpid`. A non-zero child status means exit 4.
  - **Phase C** (50 iterations): exec itself in a mode that checks one
    carried directory fd and one eventfd, then exits 0. On failure,
    exit 5.
  - **Phase D, threaded:** 4 threads × 2000 iterations racing `dup`/`close`
    on a shared wake eventfd and a shared directory fd. Afterwards the
    original fds still work (exit 6).

  After each phase, the open-fd count must equal the baseline (exits 7–10,
  one per phase). Exit 0 at the end. Build it with
  `scripts/build-guest-fixture.sh fd-stress`, and add a
  `run-process-smoke.sh` block (`expected=0`).
- [ ] **Step 2: Run it on the Task 6 core.** Expected: `exit=0`. If it
  fails, the failure is a defect in Tasks 1–6. Fix it there with its own
  failing test first, then rerun.
- [ ] **Step 3: Run it 5 times in a row.** Expected: `exit=0` all 5 times.
- [ ] **Step 4: Commit.** Message: "Add the fd layer stress gate".

### Task 8: Extra gate: SCM_RIGHTS adoption between fork relatives

**Files:**
- Modify: `src/guest_fd.c`. `guest_fd_received` adopts the object whose
  identity matches the received fd, if this process holds a live one.
  Otherwise it clears the slot.
- Create: `guest-tests/fd-rights-adopt.c`,
  `guest-tests/fixtures/fd-rights-adopt-armhf`
- Modify: `scripts/run-network-smoke.sh`, `tests/guest-fd-test.c`

**Interfaces:**
- Consumes: `guest_fd_received` (Task 4) and the identity stored by
  `guest_fd_object_new` (Task 1).
- Produces: no new API. The scope is narrow, per the spec:
  - the object was created before the fork;
  - it is sent parent → child;
  - the receiver still holds its inherited reference.

  Anything else stays a plain native fd.

- [ ] **Step 1: Write the failing tests.**
  - **Host unit `received_fd_adopts_known_object`:** attach a wake-eventfd
    object to fd A, `dup` A natively to B (simulating a received number),
    then call `guest_fd_received(B)`. `acquire(B)` returns the same object
    as A.
  - **C fixture `guest-tests/fd-rights-adopt.c`:**
    - The parent creates a wake eventfd e and a pipe p, plus an AF_UNIX
      `socketpair` s, then forks.
    - The parent sends e and `p[0]` to the child in one `SCM_RIGHTS`
      message on `s[0]`.
    - The child receives r_e and r_p on `s[1]` (exit 2 if it doesn't get
      both).
    - Writing the value 1 to r_e returns 8 (exit 3), and the child
      `_exit(0)`s after reading "ok" from r_p (exit 4).
    - The parent writes "ok" to `p[1]`, reads 8 bytes from e (exit 5),
      reaps the child with status 0 (exit 6), and exits 0.

    Build it and add a `run-network-smoke.sh` block (`expected=0`).
- [ ] **Step 2: Run them on the Task 7 core.** Expected: the unit case fails
  (`acquire(B)` is 0), and the fixture gives `exit=3` (r_e is a bare pipe
  read end, so the write fails).
- [ ] **Step 3: Implement identity-match adoption in `guest_fd_received`.**
  It must match only typed objects that are currently attached in this
  process. Native pipes are never adopted.
- [ ] **Step 4: Rerun both tests.** Expected: unit 0 failures, fixture
  `exit=0`.
- [ ] **Step 5: Final gates.**
  - The full regression: unchanged pass state.
  - The corpus: 7/11, same stages.
  - `fd-stress` 5/5.
  - Every new guest test: `exit=0`.
- [ ] **Step 6: Commit.** Message: "Adopt received fds of known objects
  between fork relatives".
- [ ] **Step 7: Update `docs/FINDINGS.md` and commit.** Add a short Phase 1
  entry:
  - the defects fixed;
  - the measured hidden base;
  - the narrow adoption limit;
  - the gate results.

  Don't touch the user's uncommitted roadmap.
