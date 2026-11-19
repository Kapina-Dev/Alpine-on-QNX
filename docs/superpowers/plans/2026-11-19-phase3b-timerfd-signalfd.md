# Phase 3, part 2: timerfd and signalfd — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Linux `timerfd` and `signalfd` as Phase 1 descriptor-layer objects
that work with read, poll/select, epoll, dup, fork and exec.

**Architecture:**
- timerfd: a per-process timer-manager thread owning one QNX kernel timer
  (`timer_create`, `SIGEV_PULSE`) per armed timerfd; expirations computed
  from the clock and added to a shared counter that reuses part 1's eventfd
  counter/readiness helpers. New file `src/linux_timerfd.c`.
- signalfd: a per-process collector thread in `sigwaitinfo` on the union of
  live signalfd masks, feeding a new process-wide pending queue that
  signalfd reads and normal delivery both consume. New file
  `src/linux_signalfd.c`; the process queue lives with the per-thread
  queues in `src/guest_thread.c`.

**Tech Stack:** C (gnu99), BB10 SDK gcc 4.6.3 on the Passport; C guest
fixtures (musl, pthreads) via `scripts/build-guest-fixture.sh`; native QNX
probes in `native-probes/`.

**Spec:** `docs/superpowers/specs/2026-11-19-phase3b-timerfd-signalfd-design.md`
(the authority for every semantic; this plan only adds decisions and pinned
values).

## Global Constraints

- **Never message io-pkt from a signal handler**, and the timer manager and
  collector use only pulses, pipes and `sigwaitinfo`/`sigtimedwait` (the
  2026-11-19 io-pkt crash).
- **Proper errors, never fake success:** unsupported cases go through
  `linux_unsupported(...)`/`linux_report_unsupported(...)` so they show in
  `LINUXEMU_DIAGNOSTICS`. Unknown Linux flag bits → `EINVAL`.
- **Descriptor rules:** a guest fd that `guest_fd_is_hidden()` reports →
  `EBADF`; new descriptors through `guest_fd_admit()`; emulator fds through
  `guest_fd_hide()` (close-on-exec, hidden range ≥ the guest's soft
  `RLIMIT_NOFILE`, 744 on the Passport).
- **Signal safety:** no `malloc`/`calloc`/`free`/stdio in dispatch paths
  (they run inside the SIGILL handler); object creation through
  `guest_fd_object_new` is the accepted exception.
- **Lock order** from part 1: fd_replace → registry → instance → table;
  `nest_lock`/`probe_lock` are leaves. New locks must be leaves or fit this
  order; say which in the report.
- **Reuse part 1's eventfd counter and readiness protocol** (lock-free
  ldrexd/strexd CAS, drain → clear → re-check) by exporting helpers from
  `src/linux_eventfd.c`; never copy them.
- **Builds are warning-free** (`-Wall -Wextra -O2 -std=gnu99 -marm`).
- **Tests:** C fixtures exit with a distinct code per check (10, 11, …) and
  print the failing check and errno on stderr; every wait is bounded by a
  watchdog with exit 100+N (BerryCore `timeout` never fires). Every test
  fails on the pre-phase core and passes on the new one: before Task 2 save
  the device's `build/linuxemu` as `build/linuxemu-pre-phase3b`.
- **Device:** the Passport, `ssh -o HostName=<passport-ip> bb10` (ask the
  user for the IP; never write it into the repo). Dev tree
  `/accounts/1000/shared/misc/linuxemu-dev` (no git): sync with
  `tar -cf - src scripts guest-tests native-probes tests | ssh … 'cd DEV && tar -xf -'`
  without `-tt`; build `sh scripts/build-native-probes.sh`; fixtures
  `ALPINE_ROOT=build/fixture-rootfs sh scripts/build-guest-fixture.sh NAME`,
  commit `guest-tests/fixtures/NAME-armhf`, add to
  `scripts/run-event-smoke.sh` (rootfs-fixture helper for self-exec tests).
  `run-full-regression.sh` needs `ssh -tt`, no device-side pipes. **Do not
  run `scripts/run-iopkt-stress.sh`.**
- **Commits:** worktree branch `phase3b-timers-signals`; one commit per
  task with device evidence (exit codes before/after); end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, no
  `Claude-Session:` trailer. Never stage
  `root-analysis/native-probe-results-20260922.md`,
  `native-probes/unix-rights-pressure.c`, `root-analysis/terminal-debug-20261014/`.
- **ARM EABI numbers and constants:**

  | Item | Value |
  | --- | --- |
  | signalfd, signalfd4 | 349, 355 |
  | timerfd_create, timerfd_settime, timerfd_gettime | 350, 353, 354 |
  | timerfd_gettime64, timerfd_settime64 | 410, 411 |
  | rt_sigsuspend | 179 |
  | CLOCK_REALTIME, MONOTONIC, BOOTTIME | 0, 1, 7 |
  | CLOCK_REALTIME_ALARM, BOOTTIME_ALARM | 8, 9 |
  | TFD_TIMER_ABSTIME, TFD_TIMER_CANCEL_ON_SET | 1, 2 |
  | TFD_NONBLOCK / SFD_NONBLOCK, TFD_CLOEXEC / SFD_CLOEXEC | 0x800, 0x80000 |
  | TFD_IOC_SET_TICKS | 0x40085400 |
  | signalfd mask size argument | 8 (else `EINVAL`) |

  `struct itimerspec` (32-bit form): two `{int32 sec, int32 nsec}`;
  time64 form: two `{int64 sec, int64 nsec}` (16-byte `timespec`).
  `struct signalfd_siginfo` is 128 bytes: `u32 ssi_signo, s32 ssi_errno,
  s32 ssi_code, u32 ssi_pid, u32 ssi_uid, s32 ssi_fd, u32 ssi_tid,
  u32 ssi_band, u32 ssi_overrun, u32 ssi_trapno, s32 ssi_status,
  s32 ssi_int, u64 ssi_ptr, u64 ssi_utime, u64 ssi_stime, u64 ssi_addr,
  u16 ssi_addr_lsb`, then zero padding to 128.

## Review Focus

1. **A process with no signalfd must behave exactly as before** (shells,
   `waitpid`/SIGCHLD flows, every existing signal test): the collector only
   exists once a signalfd is created and stops collecting when the last one
   closes. Test: Task 5 check 30 (last signalfd closed → a blocked signal
   then raised stays kernel-pending and is delivered normally on unblock),
   plus the full regression in Task 7.
2. **A tiny periodic interval** (`it_interval` = 1000 ns) must count
   overruns correctly without a pulse storm pinning a core. Test: Task 2
   check 20 (after 100 ms the count is ≥ 10000 and the manager's CPU time
   stays under 20 ms).
3. **A signal storm** (5000 `kill`s of one blocked standard signal plus 200
   queued real-time signals) must coalesce and queue correctly with no fd or
   memory growth. Test: Task 4 check 22.
4. **Out-of-range `timerfd_settime` values** (`tv_nsec` = 1000000000,
   negative seconds, far-future absolute times) → `EINVAL` or a correctly
   armed far timer, never a crash or an immediate fire. Test: Task 2 check 21.
5. **Closing a timerfd or signalfd while another thread is blocked reading
   it** must wake the reader (EBADF, as part 1 eventfd does) and leave no
   QNX timer or collector mask behind. Test: Task 3 check 15 and Task 5
   check 31.

---

### Task 1: Device probes

**Files:**
- Create: `native-probes/phase3b-probes.c`
- Modify: `scripts/build-native-probes.sh` (build it), `docs/FINDINGS.md`

**Interfaces:**
- Produces findings named exactly: `sigwait_collect`, `sigwait_siginfo`,
  `pulse_timer_accuracy`, `pulse_coalescing`, `monotonic_in_sleep`,
  `realtime_change_detect`. Tasks 2–5 branch on them.

- [ ] **Step 1: Write `phase3b-probes.c`**, one `name=value` line per
  finding:
  - `sigwait_collect=ok|fail`: threads A and B block SIGUSR1; thread C
    `sigwaitinfo`s on SIGUSR1; `kill(getpid(), SIGUSR1)` → C receives it
    within 1 s and A/B never run a handler.
  - `sigwait_siginfo=<pid,uid,code,value>`: the siginfo C sees for `kill`,
    `sigqueue` (value 42) and a fork child's `kill`.
  - `pulse_timer_accuracy=<median_us,p99_us>`: a `CLOCK_MONOTONIC` periodic
    10 ms timer with `SIGEV_PULSE` to a private channel, 200 pulses, lateness
    vs ideal; same for `CLOCK_REALTIME` absolute.
  - `pulse_coalescing=<pulses,expected>`: a 100 µs periodic timer while the
    receiver sleeps 50 ms → pulses delivered vs ideal expirations (and
    `timer_getoverrun`).
  - `monotonic_in_sleep=unknown|advances|stops`: compare MONOTONIC and
    REALTIME deltas across a `sleep 30` with the screen off if the user is
    present; otherwise `unknown`.
  - `realtime_change_detect=<method|none>`: whether any QNX mechanism
    notifies a `CLOCK_REALTIME` change; at minimum record that the
    REALTIME−MONOTONIC offset changes when `clock_settime` is called (as the
    ordinary user it may fail with EPERM; record that).
- [ ] **Step 2: Build and run on the device.** Run:
  `sh scripts/build-native-probes.sh && build/native-probes/phase3b-probes`
  Expected: six lines; every wait bounded (alarm 60).
- [ ] **Step 3: Record in `docs/FINDINGS.md`** (date, device, what, result,
  evidence), each value labelled as measured.
- [ ] **Step 4: Commit** ("Probe the Phase 3 part 2 device facts").

---

### Task 2: timerfd object and timer manager

**Files:**
- Create: `src/linux_timerfd.c`, `guest-tests/timerfd-basic.c`
- Modify: `src/linux_syscall.c` (dispatch 350, 353, 354, 410, 411),
  `src/linuxemu.h`, `src/linux_eventfd.c` (export the counter/readiness
  helpers), `scripts/run-event-smoke.sh`

**Interfaces:**
- Produces in `src/linuxemu.h`:
  - `int32_t linux_timerfd_create(int32_t clock, uint32_t flags);`
  - `int32_t linux_timerfd_settime(int fd, uint32_t flags, const void *new_spec, void *old_spec, int time64);`
  - `int32_t linux_timerfd_gettime(int fd, void *spec, int time64);`
- Produces in `src/linux_eventfd.c` (exported, used by timerfd and later
  signalfd): `int eventfd_counter_add(struct eventfd_shared *, uint64_t add);`
  `int eventfd_counter_take(struct eventfd_shared *, int semaphore, uint64_t *out);`
  `void eventfd_channel_settle(...)` — exact signatures are the builder's,
  but they wrap the existing protocol rather than duplicating it; name them
  in the report.
- Shared page (timerfd): clock, armed spec (first deadline, interval, abs
  flag), expirations-already-counted, plus the eventfd-style counter and
  channel flag.

- [ ] **Step 1: Write `guest-tests/timerfd-basic.c`:**
  - 10 setup failed.
  - 11 `timerfd_create` succeeds for clocks 0, 1, 7, 8, 9; clock 2 and 99 →
    `EINVAL`; unknown flag bit 0x4 → `EINVAL`; `TFD_CLOEXEC` sets FD_CLOEXEC.
  - 12 one-shot relative 50 ms: nonblocking read before → `EAGAIN`; after
    80 ms → 8 bytes holding 1; next read `EAGAIN`.
  - 13 absolute `CLOCK_MONOTONIC` deadline now+40 ms with
    `TFD_TIMER_ABSTIME` fires once.
  - 14 periodic 10 ms: sleep 105 ms, one read returns 10 or 11.
  - 15 `timerfd_gettime` on a 1 s timer after 200 ms reports 0.75–0.81 s
    left and the interval; a disarmed timer reports zeros.
  - 16 re-arming with `settime` resets the count (old_value returns the
    previous spec); disarm with `{0,0}` stops firing.
  - 17 read with a 7-byte buffer → `EINVAL`.
  - 18 poll/select/epoll report readability only after expiry.
  - 19 `ioctl(TFD_IOC_SET_TICKS, &5)` → next read returns 5.
  - 20 (Review Focus 2) periodic 1000 ns interval: after 100 ms the count is
    ≥ 10000 and `getrusage(RUSAGE_SELF)` system+user CPU grew < 20 ms over
    the wait.
  - 21 (Review Focus 4) `tv_nsec` 1000000000 → `EINVAL`; negative seconds →
    `EINVAL`; absolute deadline 2040-01-01 → armed, `gettime` > 0, no fire
    within 50 ms.
  - 22 the time64 calls (410/411) behave as the 32-bit ones for checks 12
    and 15.
- [ ] **Step 2: Run on the pre-phase core.** Expected: exit 11 (`ENOSYS`).
- [ ] **Step 3: Implement** the manager thread (lazy start, private channel,
  one QNX timer per armed timerfd, expirations from the clock per the spec),
  the object (pipe-pair readiness channel, hidden companions as eventfd),
  and the clock mapping (BOOTTIME and BOOTTIME_ALARM → MONOTONIC unless
  `monotonic_in_sleep=stops`; REALTIME_ALARM → REALTIME).
- [ ] **Step 4: Run the suite.** Expected: `timerfd-basic exit=0` three
  times; `run-event-smoke` 0 failures.
- [ ] **Step 5: Commit** ("Add timerfd with a per-process timer manager").

---

### Task 3: timerfd blocking, CANCEL_ON_SET, fork, exec, passing

**Files:**
- Modify: `src/linux_timerfd.c`, `src/guest_fd.c` (exec record / receive
  registration if needed)
- Create: `guest-tests/timerfd-share.c`

**Interfaces:**
- Consumes: Task 2's object and the part 1 companion/receive and exec-record
  hooks (`guest_fd_register_restore`, `guest_fd_register_receive`,
  `ops->companions`).

- [ ] **Step 1: Write `guest-tests/timerfd-share.c`:**
  - 10 setup failed.
  - 11 blocking read wakes at expiry (50 ms timer, read returns within
    40–150 ms).
  - 12 blocking read interrupted by SIGUSR1 without `SA_RESTART` → `EINTR`;
    with `SA_RESTART` it keeps waiting and returns the count.
  - 13 `fork`: the parent arms a 20 ms periodic timer; the child reads ≥ 1
    expiration; the parent's later read sees the rest (shared count).
  - 14 `exec`: a timerfd without `TFD_CLOEXEC` armed for 100 ms survives
    `execve` of the fixture itself and the new image reads 1.
  - 15 (Review Focus 5) thread B blocks in read on a disarmed timerfd;
    thread A closes it → B returns `EBADF` within 1 s; afterwards 64 new
    timerfds can be created and armed (no QNX timer leak).
  - 16 `SCM_RIGHTS`: a timerfd sent to a forked child that closed its copy
    still delivers expirations armed by the parent.
  - 17 `TFD_TIMER_CANCEL_ON_SET`, per `realtime_change_detect`: if
    detectable, an absolute REALTIME timer's blocked read fails `ECANCELED`
    after the realtime clock changes (skip with a printed reason if the
    guest can't change the clock as the ordinary user); if not detectable,
    the flag → `EINVAL` and the test checks that.
  - 18 50 timerfds with different periods all fire and count correctly
    within 10 % over 500 ms.
- [ ] **Step 2: Run on the Task 2 core.** Expected: exit 11 (blocking read
  returns `EAGAIN` or hangs until the watchdog).
- [ ] **Step 3: Implement** blocking reads (part 1's interruptible wait),
  CANCEL_ON_SET per the probe, exec record (spec + remaining time; re-arm
  in the new image), fork/`SCM_RIGHTS` via companions, and close/retire
  cleanup of the QNX timer.
- [ ] **Step 4: Run the suite** (event, signal, process smoke). Expected:
  all 0 failures; `timerfd-share` exit 0 three times.
- [ ] **Step 5: Commit** ("Share and block on timerfd").

---

### Task 4: Process-wide pending queue, collector, and signalfd read

**Files:**
- Create: `src/linux_signalfd.c`, `guest-tests/signalfd-basic.c`
- Modify: `src/guest_thread.c` (process queue next to the per-thread
  queues), `src/guest_signal.c` (`guest_signal_deliver_pending` takes from
  the process queue after the thread's), `src/linuxemu.h`,
  `src/linux_syscall.c` (dispatch 349, 355)

**Interfaces:**
- Produces in `src/linuxemu.h`:
  - `int32_t linux_signalfd(int fd, const void *mask, uint32_t size, uint32_t flags);`
  - `void guest_process_signal_queue(int linux_signal, const siginfo_t *info);`
  - `int guest_process_take_pending(const uint32_t mask[2], siginfo_t *info);`
    returns the signal number or 0.
  - `void linux_signalfd_collector_update(void);` recompute the union and
    wake the collector.

- [ ] **Step 1: Write `guest-tests/signalfd-basic.c`:**
  - 10 setup failed.
  - 11 block SIGUSR1 in all threads, `signalfd(-1, {SIGUSR1}, 8, 0)`,
    `kill(getpid(), SIGUSR1)` → read returns one 128-byte record with
    `ssi_signo` 10, `ssi_pid` == getpid(), `ssi_uid` == getuid().
  - 12 mask size 7 → `EINVAL`; unknown flag → `EINVAL`; buffer of 127 bytes
    → `EINVAL`.
  - 13 three `kill`s of SIGUSR1 before reading → one record (coalesced).
  - 14 `sigqueue` SIGRTMIN with values 1, 2, 3 → three records in order with
    `ssi_int` 1, 2, 3; one read with a 384-byte buffer returns all three.
  - 15 empty nonblocking (`SFD_NONBLOCK`) → `EAGAIN`.
  - 16 poll/epoll on the signalfd: not ready, then ready after `kill`.
  - 17 a fork child `kill`s the parent → `ssi_pid` == the child's pid.
  - 18 two signalfds with masks {SIGUSR1} and {SIGUSR1, SIGUSR2}: after one
    SIGUSR1, exactly one of the two reads returns it (the other `EAGAIN`).
  - 19 `signalfd(fd, {SIGUSR2}, 8, 0)` on an existing fd changes its mask.
  - 20 SIGKILL/SIGSTOP in the mask are accepted and ignored.
  - 21 blocking read (no `SFD_NONBLOCK`) woken by a `kill` from another
    thread after 50 ms.
  - 22 (Review Focus 3) 5000 `kill`s of SIGUSR1 plus 200 `sigqueue` of
    SIGRTMIN: one SIGUSR1 record and 200 SIGRTMIN records in order (or the
    ring depth if smaller, with the documented behaviour on overflow); open
    fd count unchanged afterwards.
- [ ] **Step 2: Run on the pre-phase core.** Expected: exit 11 (`ENOSYS`).
- [ ] **Step 3: Implement** the process queue (standard coalescing,
  real-time rings), the collector (lazy start, `sigwaitinfo` on the union,
  re-arm wake that doesn't use io-pkt, re-route signals some guest thread
  has unblocked to that thread), the signalfd object (mask + pipe-pair
  readiness, `ready_now` from the queues), and read (thread queue, then
  process queue, then zero-timeout `sigtimedwait` for the caller's
  thread-directed kernel-pending signals; Linux dequeue order).
- [ ] **Step 4: Run the suite.** Expected: `signalfd-basic exit=0` three
  times; event and signal smoke 0 failures.
- [ ] **Step 5: Commit** ("Add signalfd with a collector and a process-wide queue").

---

### Task 5: Signal semantics around the process queue

**Files:**
- Modify: `src/guest_signal.c`, `src/guest_thread.c`, `src/linux_poll.c`,
  `src/linux_epoll.c`, `src/linux_signalfd.c`
- Create: `guest-tests/signalfd-semantics.c`

**Interfaces:**
- Consumes: Task 4's queue functions and `linux_signalfd_collector_update`.

- [ ] **Step 1: Write `guest-tests/signalfd-semantics.c`:**
  - 10 setup failed.
  - 23 consumed once with a handler: SIGUSR1 collected (blocked), then
    `sigprocmask(SIG_UNBLOCK)` → the handler runs once and a signalfd read
    returns `EAGAIN`.
  - 24 a thread with SIGUSR2 unblocked and a handler gets normal delivery of
    a process-directed SIGUSR2 while a signalfd watches it; the signalfd
    sees nothing.
  - 25 default action: a child blocks SIGTERM, creates a signalfd for it,
    receives a `kill(SIGTERM)` (collected), then unblocks SIGTERM → the
    child dies of SIGTERM (`WIFSIGNALED`, `WTERMSIG` 15).
  - 26 ignored default: SIGCHLD collected, then unblocked with default
    action → nothing happens, process continues.
  - 27 masked waits: SIGUSR1 collected while blocked; then `ppoll` with a
    temporary mask unblocking SIGUSR1 returns `EINTR` at once with the
    handler run; the same for `pselect6`, `epoll_pwait` and `sigsuspend`.
  - 28 a thread already in `sigsuspend` with SIGUSR1 unblocked is woken
    when the collector queues SIGUSR1 (sent with `kill` while all other
    threads block it).
  - 29 thread-directed: thread B blocks SIGUSR1 and has a signalfd for it;
    `tgkill(B, SIGUSR1)`; B's read returns it (poll beforehand is not
    required to show it — documented limit).
  - 30 (Review Focus 1) after closing the last signalfd, `kill` of a blocked
    SIGUSR1 then unblocking delivers it once via the handler (the collector
    no longer takes it).
  - 31 (Review Focus 5) thread B blocks in read on a signalfd; thread A
    closes it → B returns `EBADF` within 1 s; the collector's union no
    longer contains its mask (a later blocked SIGUSR1 stays kernel-pending,
    as check 30).
- [ ] **Step 2: Run on the Task 4 core.** Expected: a failing check among
  23–31 (record which).
- [ ] **Step 3: Implement** default/ignore application on delivery of
  collected signals; the process-queue check and collector wake for
  `rt_sigsuspend`, `pselect6`, `ppoll`, `epoll_pwait(2)` (move
  `rt_sigsuspend` onto the signal-wait pipe where needed); collector
  re-route; close/last-close union update.
- [ ] **Step 4: Run** event, signal, process smoke. Expected: 0 failures;
  `signalfd-semantics exit=0` three times.
- [ ] **Step 5: Commit** ("Keep signal delivery exact with the process queue").

---

### Task 6: signalfd across fork, exec and SCM_RIGHTS

**Files:**
- Modify: `src/linux_signalfd.c`, `src/guest_thread.c` (fork child queue
  reset, collector restart), the `execve` path (re-raise the process queue)
- Create: `guest-tests/signalfd-share.c`

- [ ] **Step 1: Write `guest-tests/signalfd-share.c`:**
  - 10 setup failed.
  - 11 `fork`: the child's process queue is empty (a SIGUSR1 collected in
    the parent before `fork` is not read by the child); the child's own
    signalfd still works for a signal sent to the child.
  - 12 `exec`: a signalfd without `SFD_CLOEXEC` survives `execve` of the
    fixture; the new image reads a SIGUSR1 sent after exec.
  - 13 `exec` with a collected SIGUSR1 still queued: the new image's
    signalfd read returns it (re-raised; `ssi_pid` may differ — not
    checked).
  - 14 `SCM_RIGHTS`: a signalfd sent to a forked child that closed its copy
    reads signals sent to the child (or arrives as a plain pipe, per the
    builder's documented choice — the check asserts whichever the docs
    say).
- [ ] **Step 2: Run on the Task 5 core.** Expected: a failing check
  (record which).
- [ ] **Step 3: Implement** fork reset and lazy collector restart, the exec
  record (mask) and pre-`execve` re-raise, and the `SCM_RIGHTS` path.
- [ ] **Step 4: Run** event, signal, process smoke. Expected: 0 failures;
  `signalfd-share exit=0` three times.
- [ ] **Step 5: Commit** ("Carry signalfd across fork, exec and SCM_RIGHTS").

---

### Task 7: Part 2 gate and docs

**Files:**
- Modify: `docs/compatibility.md`, `docs/FINDINGS.md`,
  `docs/capability-roadmap.md` (Phase 3 part 2 status), `docs/development.md`

- [ ] **Step 1: Full regression** with `ssh -tt`:
  `sh scripts/run-full-regression.sh`. Expected:
  `linuxemu_full_regression_failures=0`.
- [ ] **Step 2: Docs.** Every documented limit from the spec: `*_ALARM`
  clocks fire only while awake; BOOTTIME mapping; timers armed by a process
  stop firing for others when it exits; thread-directed blocked signals not
  visible to poll before read; sender details lost across exec;
  `rt_sigpending`/`rt_sigtimedwait` still `ENOSYS` (follow-up); the
  CANCEL_ON_SET behaviour per the probe; the `SCM_RIGHTS` behaviour for
  signalfd.
- [ ] **Step 3: Commit** ("Record the Phase 3 part 2 gate").
