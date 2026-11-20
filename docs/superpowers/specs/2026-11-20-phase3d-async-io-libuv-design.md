# Phase 3, part 4: socket async I/O, nginx and libuv — design

Date: 2026-11-20. Roadmap: `docs/capability-roadmap.md`, "Phase 3: Linux
event interfaces", acceptance gate. Parts 1–3 (eventfd, epoll, timerfd,
signalfd, inotify) are merged on local master (`3443932`). **Approach chosen
by the user: pass socket async I/O through to QNX (option 1).** **libuv
gate meaning chosen by the user: an event-loop subset must pass; the rest of
the suite runs once and is recorded (option 1).**

## Intent

Part 4 closes the two Phase 3 gate items that are Phase 3 work:

- **nginx can serve, time out, reload and stop.** Today nginx never starts
  a worker: when it creates the master/worker channel (a socketpair) it
  calls `ioctl(FIOASYNC)` and `fcntl(F_SETOWN)` to ask for `SIGIO`, Linuxemu
  answers `ENOTTY` and `EINVAL`, and nginx gives up on the worker.
- **libuv event-loop tests pass.** Not run so far.

The third open item, Node.js, is blocked by `madvise`/`mremap` (V8 memory
and JIT) and is not part of this work. Whether it stays a Phase 3 gate item
or moves to Phase 4/6 is a separate decision for the user (see "Phase 3
after part 4").

## What the device probe found (2026-11-20, native QNX, ordinary user)

A throwaway probe on a `socketpair(AF_UNIX, SOCK_STREAM)`:

- `ioctl(FIOASYNC, &1)` returns 0.
- `fcntl(F_SETOWN, getpid())` returns 0; `F_GETOWN` returns a value (not yet
  checked against the pid).
- `F_GETFL` already shows QNX `O_ASYNC` (0x10000) after `FIOASYNC`;
  `F_SETFL | O_ASYNC` returns 0.
- A write on the peer **delivers a real `SIGIO`** (QNX 22) to the owner.

On a pipe: `ioctl(FIOASYNC)` returns 0 but **no `SIGIO` arrives**, and
`F_SETOWN` fails with `ENOSYS`. The existing Linuxemu comment ("QNX answers
`ENOSYS`") describes pipes, not sockets.

Linuxemu already translates host signal 22 to Linux `SIGIO` (29)
(`host_signal_to_linux` in `src/linux_syscall.c`). Today `F_SETFL` keeps
only `O_APPEND` and `O_NONBLOCK` and silently drops Linux `O_ASYNC`
(0x2000); `F_SETOWN`/`F_GETOWN` are reported as unsupported `EINVAL`
(`fcntl-owner`); `FIOASYNC` (0x5452) falls to `ENOTTY` without a diagnostic.

libuv is not installed in the test rootfs; Alpine packages it (`libuv`,
`libuv-dev`) without its test runner.

## Goals

1. Socket async I/O works as on Linux: `ioctl(FIOASYNC)`, `fcntl(F_SETFL,
   O_ASYNC)`/`F_GETFL`, `fcntl(F_SETOWN)`/`F_GETOWN` on sockets, with the
   guest receiving Linux `SIGIO` (29) when the socket becomes ready.
2. nginx passes the corpus and the gate's four behaviours: serve, time out
   (an idle client is closed by nginx's own timeout), reload (`-s reload`
   swaps workers and keeps serving), stop (`-s quit`, already a stage).
3. libuv's event-loop subset (below) passes on the Passport through a
   repeatable script; the full upstream suite runs once and its results are
   recorded.
4. Every gap found by the libuv subset that is Phase 3 event-interface work
   is fixed in this part.

## Non-goals

- `SIGIO` for pipes, FIFOs, ttys, regular files, or Linuxemu's own typed
  objects (eventfd, epoll, timerfd, signalfd, inotify). QNX can't deliver it
  for pipes, and nothing needs it yet.
- `F_SETSIG`/`F_GETSIG`, `F_SETOWN_EX`/`F_GETOWN_EX`, and `SIGURG` for
  out-of-band data.
- A Linuxemu-generated `SIGIO` (a readiness-watching thread) — approach 3,
  rejected.
- Making the whole libuv suite pass; fixing Node.js's start-up crash.
- io_uring (libuv probes it and falls back on `ENOSYS`; that stays).

## Anti-goals

- A call that claims async I/O works and never delivers the signal. Where
  QNX can't do it, the guest gets an error and `LINUXEMU_DIAGNOSTICS` a line.
- Quietly shrinking the libuv subset to make the gate pass. A subset test
  leaves the subset only with the user's OK, recorded with its reason.
- Regressing the existing corpus apps or the full regression.

## Constraints

- All code goes through the existing paths: `ioctl` mapping in
  `src/linux_termios.c`, `fcntl` in `src/linux_syscall.c`, signal
  translation and default actions in `src/guest_signal.c`, diagnostics via
  `linux_report_unsupported`.
- Linux constants (ARM EABI): `FIOASYNC` 0x5452, `O_ASYNC` 0x2000 (octal
  020000), `F_SETOWN` 8, `F_GETOWN` 9, `F_SETSIG` 10, `F_GETSIG` 11,
  `F_SETOWN_EX` 15, `F_GETOWN_EX` 16, `SIGIO` 29. QNX: `O_ASYNC` 0x10000,
  `SIGIO` 22.
- Device workflow as in parts 1–3 (`ssh bb10`; dev checkout
  `/accounts/1000/shared/misc/linuxemu-dev`; native probes built with the
  SDK as `scripts/build-native-probes.sh` does). Don't run
  `run-iopkt-stress.sh`. Never write the device's address into the repo.
- Downloaded sources are pinned by version and sha256, like
  `scripts/fetch-alpine-test-rootfs.sh`.

## Approaches considered

1. **Pass socket async I/O through to QNX** (chosen). Sockets forward to
   QNX; QNX's `SIGIO` is translated to Linux 29. Everything else returns an
   error with a diagnostic. Real behaviour, small code.
2. **Accept and record, never signal.** Cheapest and probably enough for
   nginx, whose `SIGIO` handler only sets a flag — but a silent lie.
3. **Linuxemu generates `SIGIO` itself** for any fd from a readiness
   thread. Covers pipes, but a thread, races and fork/exec handling for no
   current consumer.

## Design

### Which descriptors

"Supported" means a host descriptor whose `fstat` is `S_ISSOCK`, which has
no Linuxemu typed object, **and whose socket kind (family and type, from
`getsockopt` `SO_TYPE` and the address family) the probe showed QNX
actually signals for** (my call: an allow-list from the probe results).
A socket kind QNX accepts but never signals for is refused like a pipe.
Everything else is "not supported for async I/O".

### ioctl(FIOASYNC)

Read the guest `int`. Socket: forward to QNX `FIOASYNC` and return its
result. Not a socket: `EINVAL` through `linux_report_unsupported` with
detail "async I/O (FIOASYNC) is only supported on sockets". A bad pointer
gives `EFAULT`.

### fcntl(F_SETFL, O_ASYNC) and F_GETFL

`F_SETFL` additionally honours Linux `O_ASYNC` (0x2000): on a socket, set
or clear QNX `O_ASYNC` together with the other flags. On anything else,
setting `O_ASYNC` returns `EINVAL` with a diagnostic and changes nothing;
clearing it (or leaving it clear) is accepted as today. `F_GETFL` maps QNX
`O_ASYNC` back to Linux 0x2000, so `FIOASYNC` and `F_SETFL` agree.

### fcntl(F_SETOWN/F_GETOWN)

Socket: forward to QNX. A positive argument is a process ID, a negative one
a process group (Linux semantics; the probe checks QNX agrees). Guest
process IDs are QNX process IDs, so no translation is needed — the plan
verifies this. `F_GETOWN` returns QNX's value; the probe checks it equals
what was set. If QNX reports the owner in a different but consistent form,
translate it back; if a process-group owner doesn't work on QNX, refuse a
negative argument with `EINVAL` and a diagnostic (my calls). Not a socket: keep today's reported `EINVAL` (`fcntl-owner`),
with the detail updated to say sockets are supported.

### SIGIO delivery

Host `SIGIO` (22) is already translated to Linux 29. The plan confirms with
a fixture that it reaches a guest handler, that `SA_SIGINFO` handlers get a
sane `siginfo` (`si_signo` 29; `si_code`/`si_fd` best effort, documented),
and that the guest default action for an unhandled `SIGIO` is Linux's
(terminate), not QNX's.

### fork, exec, close

The owner and async flag live in the QNX socket, so they carry across
`fork`, `exec`, `dup` and `SCM_RIGHTS` the way QNX does; the probe records
what QNX does and the docs say so. No Linuxemu state is added.

### nginx stages

The corpus harness has fixed stages (install, startup, basic, concurrent,
shutdown). The gate's extra behaviours go inside nginx's existing stages
(my call): the **timeout** check in `basic` (a client that connects and
sends nothing is closed by nginx after a short `client_header_timeout`,
e.g. 2 s, within a bound), and the **reload** check in `concurrent` (`nginx
-s reload` after the parallel GETs, then a GET still answers and the worker
PID changed). `shutdown` already covers stop.

### libuv

- Fetch the **upstream libuv source of the version Alpine's `libuv`
  package has in the test rootfs's Alpine release** (read from the existing
  pinned manifests/package index), pinned by sha256, on the laptop
  (`scripts/fetch-*.sh` style).
- Build libuv **statically from that source** together with upstream's
  test runner, inside the guest with the guest's gcc, with a script
  (`scripts/build-libuv-tests.sh`) (my call: upstream tests use internal
  `uv__*` symbols and headers that Alpine's shared `libuv.so` hides, so
  linking against it would make some tests impossible to build). The
  binary is cached under `build/`, not committed.
- The build tools (gcc, musl-dev, make) come from a **pinned package
  closure of their own** with an apk sha256 manifest, installed and removed
  by the script the way the corpus does it, **outside the corpus harness**
  (my call), so the corpus stays 11 apps.
- A new suite script (`scripts/run-libuv-smoke.sh`) runs the **subset** one
  test at a time with a per-test timeout and prints
  `linuxemu_libuv_failures=N`. It is added to the full regression.
- **Subset** (exact test names pinned in the plan from that version's
  `test/test-list.h`): timers (`timer_*`), idle/prepare/check and loop
  lifecycle (`idle_*`, `loop_*`, `ref`/`unref` basics), async wakeups
  (`async*`), poll handles (`poll_duplex`, `poll_unidirectional`,
  `poll_close`, and similar), threadpool work (`threadpool_*`), signals
  (`signal_*`, `we_get_signals*`), process spawn and exit (`spawn_*` that
  use only pipes and exit codes), pipes on loopback (`pipe_*` that use a
  path in a writable directory), TCP on 127.0.0.1 (`tcp_ping_pong`,
  `tcp4_echo`-style ping-pong, `tcp_close*`, `tcp_shutdown*`), and file
  watching (`fs_event_*`, given part 3's documented limits).
- A test whose failure is caused by something outside Phase 3 (IPv6, a
  missing `/proc` entry, a TTY detail) is not removed silently. The
  executor puts it on a **"needs the user's OK" list** with its cause,
  finishes everything else, and brings the list to the user at the gate;
  only approved removals leave the subset, and they are recorded in
  `docs/corpus-results.md` (my call: execution doesn't halt mid-plan). The
  gate is "0 failures in the subset after approved removals".
- The **full suite** runs once at the gate; pass/fail/skip counts and each
  failing test's one-line cause go into `docs/corpus-results.md`.

#### Fixing what nginx and libuv hit

Any Linuxemu gap that nginx's worker, timeout or reload paths, or the libuv
subset, run into after the async-I/O fix is in scope when it is
event-interface, signal, socket or process work of the kind Phases 1–3
cover (my call). A gap clearly belonging to a later phase (V8/JIT memory,
Android layers) goes on the same "needs the user's OK" list instead.

### Diagnostics and names

New detail strings for the refused cases above; no new syscall numbers.

## Tests

- **Probe (first plan task):** native QNX, extending the throwaway probe:
  `AF_UNIX` stream and datagram, TCP and UDP on loopback, a listening
  socket (accept readiness), a pty, a pipe; who gets `SIGIO` (owner pid,
  process group, a thread), `F_GETOWN` value, behaviour across `fork` and
  `exec`, `SIGIO` on peer close, and `F_SETFL O_ASYNC` without `FIOASYNC`.
  Results go to `docs/FINDINGS.md`.
- **Fixture `async-io`** (`guest-tests/async-io.c`): `FIOASYNC` and
  `O_ASYNC` on a socketpair deliver `SIGIO` to a handler on peer write; `F_GETFL`
  shows `O_ASYNC`; `F_SETOWN`/`F_GETOWN` round-trip; clearing stops
  signals; refused cases (pipe, eventfd, regular file) give `EINVAL`; a
  bad `FIOASYNC` pointer gives `EFAULT`; owner and flag after `fork` and
  `exec` match what the probe recorded. Added to `run-event-smoke`.
- **nginx** corpus app passes all stages including the new timeout and
  reload checks.
- **libuv** subset passes via `run-libuv-smoke`.

## Gate for part 4

- Fixture `async-io` passes; `run-libuv-smoke` reports 0 failures.
- Corpus: nginx compatible (with timeout and reload checks), no regression
  elsewhere (so 10 of 11 with Node.js still failing).
- Full device regression `linuxemu_full_regression_failures=0`, now
  including the libuv subset.
- Docs: `docs/compatibility.md` ("Limits of the Phase 3 part 4 calls"),
  `docs/FINDINGS.md`, `docs/corpus-results.md` (with the full libuv suite
  results), the roadmap's Phase 3 status, `docs/development.md`, README.

## Phase 3 after part 4

With part 4 done, the only open Phase 3 gate item is Node.js, which is
blocked by V8 memory work. The roadmap status says so and does not mark
Phase 3 complete unless the user decides to move the Node.js item to
Phase 4/6.

## Decided by the user

- Approach 1: pass socket async I/O through to QNX.
- libuv gate: an event-loop subset must pass; the full suite runs once and
  is recorded.
- Same flow as parts 2 and 3: spec, plan with Global Constraints,
  subagent-driven development in a worktree branch.
- Don't run `run-iopkt-stress.sh`.

## My calls (correct me)

- Non-socket async I/O returns `EINVAL` with a diagnostic rather than
  succeeding silently.
- `F_SETSIG`, `F_*OWN_EX` and `SIGURG` stay out.
- nginx's timeout and reload checks live inside its existing corpus stages.
- The libuv tests are built inside the guest against Alpine's own
  `libuv.so`, by script, cached, not committed; the subset runs in the full
  regression.
- Removing a test from the libuv subset needs the user's OK; such tests are
  collected on a list and brought to the user at the gate, not halting
  execution.
- Async I/O is forwarded only for socket kinds the probe shows QNX signals
  for; the others are refused.
- libuv is built statically from the pinned upstream source (not linked to
  Alpine's `libuv.so`); its build tools come from their own pinned closure
  outside the corpus.
- Gaps nginx and the libuv subset hit after the async-I/O fix are fixed in
  this part when they are Phase 1–3-kind work.

## Left to the builder

- The exact subset test names (from the pinned version's test list) and
  per-test timeouts.
- How the nginx reload check finds the worker PID (pid file plus a process
  listing, or another method that works in the guest).
- How the "is a socket" check is shared between `ioctl` and `fcntl`.
- Fixture layout and task order after the probe task.
