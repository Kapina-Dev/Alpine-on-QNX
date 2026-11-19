# Phase 3, part 2: timerfd and signalfd — design

Date: 2026-11-19. Roadmap: `docs/capability-roadmap.md`, "Phase 3: Linux
event interfaces" (timerfd, signalfd). Part 1 (eventfd, epoll) is merged and
pushed (`37443ed`). Approach chosen by the user: **signalfd through a
collector thread (A); timerfd through a per-process timer manager.**

## Intent

Part 2 adds Linux `timerfd` and `signalfd` as objects in the Phase 1
descriptor layer, so they work with `read`, `poll`/`select`, epoll, `dup`,
`fork`, `exec` and close the way part 1's eventfd does. The user follows
the roadmap phases in order and will release once after part 3 (inotify).

No corpus app calls either interface today. The concrete consumers are the
Android track (AlarmManager uses timerfd when `/dev/alarm` is absent,
including `CLOCK_BOOTTIME` and the `*_ALARM` clocks; init-style daemons use
signalfd) and the roadmap's Phase 3 gate. **The user chose guest tests as
the proof**: no new corpus workload.

## Goals

- `timerfd_create`, `timerfd_settime`, `timerfd_gettime` and their time64
  forms with Linux semantics: one-shot and periodic timers, relative and
  `TFD_TIMER_ABSTIME` deadlines, the 8-byte expiration count on `read`
  (accumulated overruns), `TFD_NONBLOCK`, `TFD_CLOEXEC`, blocking and
  nonblocking reads, readiness, `dup`, `fork`, `exec`, close races,
  `TFD_IOC_SET_TICKS`, and `TFD_TIMER_CANCEL_ON_SET` (or a deliberate
  Linux error, decided by a device probe).
- `signalfd` and `signalfd4` with Linux semantics: the 128-byte
  `signalfd_siginfo` records, standard-signal coalescing, real-time
  ordering and queueing, sender information, blocking and nonblocking reads,
  polling, several signalfds, updating a signalfd's mask, thread-directed
  delivery, `SFD_NONBLOCK`, `SFD_CLOEXEC`, `dup`, `fork`, `exec`.
- A signal is consumed exactly once: either by normal guest delivery or by a
  signalfd read, never both (roadmap requirement).
- Every existing suite stays green; the full device regression passes with 0
  failures.

## Non-goals

- inotify (part 3).
- A corpus workload for these calls (user's choice: guest tests suffice).
- Moving all signal blocking into Linuxemu (approach B below). That belongs
  to Phase 6, "process and synchronization depth".
- Isolating the io-pkt crash found on 2026-11-19 (separate follow-up).

## Anti-goals

- Linuxemu must never send io-pkt a message from a signal handler (the
  2026-11-19 io-pkt crash). The collector and timer manager use pulses,
  pipes and `sigwaitinfo` only.
- No per-timer signal handlers (roadmap: "use a central timer manager rather
  than one asynchronous signal handler per timer").
- No polling loops that wake the CPU on a fixed tick when nothing is armed.
- Success stubs: a case Linuxemu can't honour returns a Linux error and is
  documented.

## Constraints (from the code and earlier phases)

- **Signal model today** (`src/guest_signal.c`, `src/guest_thread.c`): the
  guest's signal mask is applied to the host thread (`pthread_sigmask`), so
  a signal the guest blocks stays blocked on QNX. The host handler queues a
  signal it can't deliver at once into the *receiving thread's* pending
  state (`guest_thread_signal_pending`: a bitmap plus saved siginfo for
  standard signals, a ring per real-time signal). After every syscall
  `trap.c` calls `guest_signal_deliver_pending`, which takes unblocked
  signals from that queue. There is no process-wide queue.
- **Consequence:** a process-directed signal that every guest thread blocks
  never reaches Linuxemu; it stays pending in the QNX kernel. That is
  exactly what a signalfd program wants to read.
- Part 1 machinery to reuse: the eventfd shared counter and readiness pipe
  (`src/linux_eventfd.c`), the fd layer's companions/exec-record/receive
  hooks, epoll's `ready_now`, and each thread's persistent hidden
  signal-wait pipe.
- Hidden fd budget: `HIDDEN_RESERVE` 256 per process (guest
  `RLIMIT_NOFILE` 744 on the Passport).
- No `malloc`/stdio in dispatch paths (syscalls run inside the SIGILL
  handler), warning-free builds, a distinct exit code per test check, every
  test wait bounded (BerryCore `timeout` never fires).

## Approaches considered

**signalfd (the real choice):**

- **A. Collector thread (chosen).** One emulator thread per process waits in
  `sigwaitinfo` on the union of all live signalfds' masks. QNX hands it the
  process-directed signals that every guest thread blocks. It moves each one
  into a new **process-wide pending queue** (same shape as the per-thread
  queues) and raises the readiness of every signalfd whose mask includes it.
  `read` on a signalfd takes from the calling thread's queue first, then the
  process queue; normal delivery (`guest_signal_deliver_pending`) also takes
  from the process queue, so a signal is consumed once. Existing delivery
  paths stay as they are.
- **B. Move all blocking into Linuxemu.** Keep host signals unblocked, track
  the guest mask only in emulator state and queue everything ourselves.
  Exact, but it rewrites the core signal model every suite depends on.
  Deferred to Phase 6.
- **C. Poll pending signals on a short timer.** Rejected: latency and
  constant wake-ups.

**timerfd: one sensible way.** A per-process timer manager thread with one
QNX kernel timer (`timer_create`, `SIGEV_PULSE`) per armed timerfd, all
delivering pulses to the manager's private channel. Rejected alternatives:
one signal handler per timer (roadmap forbids it); the manager sleeping to
its own nearest deadline (less accurate than kernel timers for the same
code).

## Design: signalfd

**Object.** `GUEST_FD_SIGNALFD` (already reserved). Per object: the
signalfd's Linux signal mask and a readiness pipe (guest fd = read end;
write end hidden). `SIGKILL` and `SIGSTOP` in the mask are silently ignored,
as on Linux.

**Collector thread.** Started lazily by the first `signalfd` call in a
process; restarted lazily in a `fork` child and after `exec`.

- It waits in `sigwaitinfo` (or `sigtimedwait`) on the union of every live
  signalfd's mask. When a signalfd is created, its mask changes, or the last
  reference to one closes, the union changes and the collector is woken to
  re-arm with the new set (mechanism is the builder's call; it must not use
  io-pkt).
- **It must not steal a signal some guest thread would take normally.** On
  Linux a signal only stays pending, and so visible to signalfd, when the
  receiving thread or every thread blocks it. If the collector receives a
  signal that some guest thread currently has unblocked, it hands it to that
  thread (queue into that thread's pending state and wake it so it's
  delivered) instead of the process queue.
- Every signal it takes goes into the process-wide queue with its siginfo
  (sender pid/uid, `si_code`, value for `sigqueue`), and every signalfd
  whose mask contains it gets a readiness byte.

**Process-wide queue.** Standard signals coalesce (one pending instance,
the first siginfo kept); real-time signals queue in order, with the same
ring depth as the per-thread queues. `guest_signal_deliver_pending` and
`rt_sigpending`/`rt_sigtimedwait` (wherever the code implements them) look
at the process queue after the thread's own.

**read.** Fills as many 128-byte `signalfd_siginfo` records as fit
(`EINVAL` if the buffer is under 128 bytes), taking signals in the
signalfd's mask from the calling thread's queue, then the process queue,
lowest standard signal first and real-time in order, as Linux dequeues.
Thread-directed signals the caller blocks that are still in the QNX kernel
(not yet in any queue) are also collected for the calling thread at read
time with a zero-timeout `sigtimedwait` on the signalfd's mask. Empty: `EAGAIN`
when nonblocking, else block until a matching signal arrives (on the
readiness pipe plus the thread's signal-wait pipe, interruptible like part
1's eventfd waits). After a read empties the matching queues, the readiness
byte is drained; readiness must never be lost (same discipline as eventfd).

**Default and ignored actions.** Today the per-thread queues only ever
hold signals that have a guest handler (the host handler is installed only
when the guest sets one). The collector takes signals regardless of their
action, so whenever a collected signal ends up delivered normally (the guest
unblocks it, or the collector re-routes it to a thread that has it
unblocked) and the guest's action is the default or ignore, Linuxemu
applies that action itself: ignore discards it; a default that terminates
or stops the process is carried out the way the existing self-signal path
does it (restore the host default and re-raise). The sender's details are
lost in that case, which only matters for a process that is ending.

**Every masked wait sees the process queue.** Collection must never leave
a thread waiting for a signal Linux would already deliver. So `rt_sigsuspend`,
`pselect6`, `ppoll` and `epoll_pwait`/`epoll_pwait2` check the process
queue against their temporary mask before sleeping (returning `EINTR` with
delivery as today if one matches), and the collector, after queueing a
signal, wakes any thread whose current or temporary mask leaves it
unblocked, through that thread's signal-wait pipe. `rt_sigsuspend` moves
onto that pipe protocol instead of the host `sigsuspend` where needed for
this. `rt_sigpending` and `rt_sigtimedwait` stay unimplemented (`ENOSYS`,
as today) and are listed as a follow-up; whoever adds them must include the
process queue.

**exec.** Signals waiting in the process queue at `execve` are re-raised to
the process just before the host `execve`, so they sit pending in the QNX
kernel (the blocked mask survives exec) and the new image's collector or
delivery picks them up. Sender details and `sigqueue` values are lost
across `exec` (documented); the per-thread queues already behave this way.

**Readiness.** `poll_fd` is the pipe read end; `ready_now` also reports
`POLLIN` when the process queue or the calling thread's queue holds a
signal in the mask, so a byte lost to a race can't hide a pending signal.

**Limit (documented):** a signal sent to one specific thread (`tgkill`)
that the thread blocks stays in the QNX kernel until that thread reads its
signalfd; `read` returns it, but `poll`/epoll don't show it beforehand.

**fork / exec / passing.** A `fork` child keeps its signalfds (mask and
pipe inherited), starts its own collector on first need, and has an empty
process queue (Linux: pending signals aren't inherited). `exec` carries the
mask in the exec record; the new image restores the object and restarts the
collector. A signalfd received over `SCM_RIGHTS` is rebuilt from its mask
(a companion carrying it, or arrives as a plain pipe and is documented, the
builder's call).

## Design: timerfd

**Object.** `GUEST_FD_TIMERFD` (already reserved). Shared state in one page
of unnamed shm, like eventfd: the clock id, the armed spec (value,
interval, absolute flag), the expiration count, and the readiness channel
flag. The count and readiness follow part 1's lock-free counter and
channel protocol (reuse its helpers rather than copying them).

**Timer manager.** One emulator thread per process, started lazily, with a
private QNX channel. Each armed timerfd owns a QNX timer
(`timer_create(clock, SIGEV_PULSE to the channel)`); `timerfd_settime` arms,
re-arms or disarms it. On a pulse the manager computes expirations from the
clock (`floor((now - first_deadline) / interval) + 1` minus what was
already counted), so coalesced or late pulses never lose or double-count
overruns; it adds them to the shared count and raises readiness.

**Clocks.** `CLOCK_REALTIME` and `CLOCK_MONOTONIC` map directly.
`CLOCK_BOOTTIME` maps to `CLOCK_MONOTONIC` unless a device probe shows QNX's
monotonic clock stops during device sleep (then document it).
`CLOCK_REALTIME_ALARM` and `CLOCK_BOOTTIME_ALARM` map to `CLOCK_REALTIME` and
the `CLOCK_BOOTTIME` mapping (user's choice, 2026-11-19): they fire on time
while the device is awake but cannot wake it from sleep (documented limit).
Reason: Android's AlarmManager creates timerfds on all five clocks and
abandons the timerfd path if any fails, so `EINVAL` would keep it off
timerfd. Unknown clocks and flags return `EINVAL`.

**settime / gettime.** Linux validation (`EINVAL` for bad flags, negative or
too-large `tv_nsec`); `old_value` returns the previous setting; `gettime`
returns the time left and the interval. Re-arming resets the count, as on
Linux.

**read.** 8-byte count, then reset to 0; `EINVAL` under 8 bytes; empty:
`EAGAIN` if nonblocking, else block interruptibly (part 1's eventfd wait).
`TFD_IOC_SET_TICKS` sets the count (and raises readiness when non-zero).

**`TFD_TIMER_CANCEL_ON_SET`.** With an absolute `CLOCK_REALTIME` timer,
Linux fails a pending read with `ECANCELED` when the realtime clock is set.
A device probe decides how: if QNX can notify a clock change, use it; if
not, detect it by comparing the realtime-minus-monotonic offset at each
manager wake-up and at read time, and document that a change is only seen
then. If neither works reliably, return `EINVAL` for the flag and document.

**fork / exec / passing.** The shared page makes the count shared after
`fork` (as on Linux), but only the creating process's manager fires the
timer: a timer armed in the parent keeps firing for the child while the
parent lives; a timer *re-armed* in the child gets a child-side manager and
QNX timer. If the parent exits, a parent-armed timer stops firing for the
child (documented limit). `exec` carries the spec and the remaining time in
the record; the new image re-arms. `SCM_RIGHTS` passing uses part 1's
companion mechanism (shm + channel), with the same "fires only while the
arming process lives" limit.

## Tests (guest fixtures, `scripts/run-event-smoke.sh`)

- **timerfd:** create with every clock (including both `*_ALARM` clocks, which must succeed) and reject unknown ones; one-shot
  relative and absolute; periodic with overruns (sleep through several
  intervals and read the accumulated count); disarm; re-arm resets the
  count; `gettime` time-left accuracy within a tolerance; nonblocking
  `EAGAIN`; blocking read woken by expiry and interrupted by a signal
  (`EINTR` and `SA_RESTART`); poll/select/epoll readiness; `dup`; `fork`
  (child reads the parent's expirations); `exec` carry-over;
  `TFD_IOC_SET_TICKS`; `CANCEL_ON_SET` per the probe's outcome; close while
  another thread blocks in read; many timers at once (e.g. 50) all firing.
- **signalfd:** read of a blocked process-directed signal (`kill` to self
  with the signal blocked in every thread); several standard signals
  coalesce; real-time signals queue in order with their `sigqueue` values;
  sender pid/uid in the record; buffer smaller than 128 bytes → `EINVAL`;
  multiple records per read; nonblocking `EAGAIN`; blocking read woken by a
  signal from another process; poll/epoll readiness; two signalfds with
  overlapping masks (each signal read once in total); changing the mask with
  `signalfd(fd, ...)`; a thread with the signal unblocked still gets normal
  delivery and the signalfd sees nothing; "consumed once": unblocking after
  queueing delivers it once and the signalfd then sees nothing (with a
  handler, with `SIGTERM`'s default action ending the process, and with
  `SIGCHLD`'s default ignore); a thread in `sigsuspend`/`ppoll` with a
  collected signal unblocked returns at once; thread-directed blocked signal returned by that thread's read; `fork`
  (child's empty queue) and `exec` carry-over.
- The full device regression and the event, signal and process suites.

## Device probes (first task)

- Does QNX `sigwaitinfo` in a dedicated thread receive a process-directed
  signal that every other thread blocks, and with full siginfo
  (pid, uid, code, value)?
- `timer_create` with `SIGEV_PULSE` on `CLOCK_REALTIME` and
  `CLOCK_MONOTONIC`: accuracy and pulse coalescing under load; whether
  `CLOCK_MONOTONIC` advances during device sleep (as far as testable).
- A notification or reliable detection for realtime clock changes
  (`TFD_TIMER_CANCEL_ON_SET`).
- Findings go into `docs/FINDINGS.md`.

## Gate for part 2

All new guest tests pass on the Passport; the full device regression passes
with 0 failures; `docs/compatibility.md`, `docs/FINDINGS.md`, the roadmap's
Phase 3 status and `docs/development.md` updated, including every documented
limit above.

## Decided by the user

- Guest tests are the proof; no corpus workload for part 2.
- signalfd uses approach A, a collector thread with a process-wide queue.
- One release after part 3.
- `*_ALARM` clocks map to the plain clocks (awake-only firing, can't wake
  the device), so AlarmManager can use timerfd.

## My calls (correct me)

- timerfd uses a per-process manager thread with one QNX kernel timer per
  armed timerfd, delivering pulses; expirations are computed from the clock.
- `CLOCK_BOOTTIME` → `CLOCK_MONOTONIC` unless the probe says otherwise.
- A timer keeps firing for a `fork` child only while the arming process
  lives.
- The collector never takes a signal some guest thread has unblocked; it
  re-routes it to that thread.
- Thread-directed blocked signals are visible to that thread's signalfd
  `read` but not to `poll` beforehand (documented limit).
- Collected signals with a default or ignore action get that action applied
  by Linuxemu when they're delivered normally.
- `rt_sigsuspend`, `pselect6`, `ppoll`, `epoll_pwait(2)` check the process
  queue and are woken by the collector; `rt_sigpending`/`rt_sigtimedwait`
  stay `ENOSYS` (follow-up).
- Process-queue signals are re-raised before `execve`; sender details are
  lost across exec.

## Left to the builder

- How the collector is woken to re-arm with a new mask union (not via
  io-pkt).
- The exec record and `SCM_RIGHTS` formats for both objects.
- Data structures and task order.
