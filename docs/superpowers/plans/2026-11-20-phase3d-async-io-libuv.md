# Phase 3, part 4: socket async I/O, nginx and libuv — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the Phase 3 gate's nginx and libuv items: Linux socket
async I/O (`FIOASYNC`, `O_ASYNC`, `F_SETOWN`/`F_GETOWN`, `SIGIO`) passed
through to QNX, nginx serving/timing out/reloading/stopping in the corpus,
and libuv's event-loop test subset passing on the Passport.

**Architecture:** For socket kinds the device probe shows QNX signals for,
Linuxemu forwards `FIOASYNC`, `O_ASYNC` and `F_SETOWN`/`F_GETOWN` to QNX and
the existing host→Linux signal translation turns QNX `SIGIO` (22) into Linux
29. Every other descriptor is refused with `EINVAL` and a diagnostic. libuv
is built statically from pinned upstream source inside the guest, and a new
suite runs a pinned event-loop subset of its tests.

**Tech Stack:** C (gnu99), BB10 SDK gcc 4.6.3 on the Passport; C guest
fixtures (musl) via `scripts/build-guest-fixture.sh`; native QNX probes in
`native-probes/`; POSIX sh scripts; Alpine 3.24 packages; upstream libuv.

**Spec:** `docs/superpowers/specs/2026-11-20-phase3d-async-io-libuv-design.md`
(the authority for every semantic and device fact; this plan adds
decisions and pinned values).

## Global Constraints

- **Proper errors, never fake success:** every refused case returns a Linux
  error through `linux_report_unsupported(number, linux_error, detail,
  args)` so it shows in `LINUXEMU_DIAGNOSTICS`. Never accept async I/O that
  will not signal.
- **Linux constants (ARM EABI):** `FIOASYNC` 0x5452, `O_ASYNC` 0x2000,
  `F_GETFL` 3, `F_SETFL` 4, `F_SETOWN` 8, `F_GETOWN` 9, `SIGIO` 29.
  QNX: `O_ASYNC` 0x10000, `SIGIO` 22. Syscall numbers: `ioctl` 54,
  `fcntl64` 221 (the existing `fcntl-owner` report uses 221).
- **Supported descriptor:** `S_ISSOCK`, no Linuxemu typed object, and a
  socket kind on the probe's allow-list (Task 1). One shared predicate,
  `int linux_socket_async_capable(int fd)`, is the only place that decides.
- **Out of scope:** `F_SETSIG`/`F_GETSIG`, `F_SETOWN_EX`/`F_GETOWN_EX`,
  `SIGURG`, SIGIO for non-sockets, a Linuxemu-generated SIGIO, io_uring.
- **"Needs the user's OK" list:** a libuv subset test or nginx failure whose
  cause is not Phase 1–3-kind work (event interfaces, signals, sockets,
  processes) is not fixed and not dropped: append it with its cause to
  `docs/superpowers/plans/2026-11-20-phase3d-needs-ok.md` (create on first
  use) and carry on. Only the user removes items from the subset.
- **Descriptor and signal-safety rules** from parts 1–3: hidden guest fd →
  `EBADF`; no `malloc`/stdio in dispatch paths; lock order fd_replace →
  registry → instance → table, and this part adds no locks.
- **Builds are warning-free** (`-Wall -Wextra -O2 -std=gnu99 -marm`).
- **Tests:**
  - C fixtures exit with a distinct code per check (10, 11, …), print the
    failing check and errno on stderr, and bound every wait with a watchdog
    that exits 100+N.
  - Every new fixture check fails on the pre-phase core and passes on the
    new one: before Task 2, save the device's `build/linuxemu` as
    `build/linuxemu-pre-phase3d`.
  - A signal is "delivered" if the handler runs within 2 s; "not delivered"
    means no handler run within 500 ms.
- **Device:**
  - `ssh -o HostName=<address> bb10`; the controller gives the address in
    the dispatch. **Never write any IP address into the repo** (grep the
    staged diff for `192\.168` before each commit; zero hits).
  - Dev tree `/accounts/1000/shared/misc/linuxemu-dev` (no git). Sync with
    `tar -cf - src scripts guest-tests native-probes docs/corpus | ssh … 'cd DEV && tar -xf -'`
    (no `-tt`).
  - Build with `sh scripts/build-native-probes.sh`.
  - Fixtures: `ALPINE_ROOT=build/fixture-rootfs sh scripts/build-guest-fixture.sh NAME`;
    commit `guest-tests/fixtures/NAME-armhf`.
  - `run-full-regression.sh` and `run-corpus.sh` need `ssh -tt`.
  - **Do not run `scripts/run-iopkt-stress.sh`.**
- **Commits:** worktree branch `phase3d-async-libuv`; one or more focused
  commits per task with device evidence; messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` and no
  `Claude-Session:` trailer. Never stage
  `root-analysis/native-probe-results-20260922.md`,
  `native-probes/unix-rights-pressure.c` or
  `root-analysis/terminal-debug-20261014/`.

## Review Focus

1. **Toggling `O_ASYNC` with `F_SETFL` must keep `O_NONBLOCK`/`O_APPEND`**
   (and clearing `O_NONBLOCK` must not clear `O_ASYNC`) — nginx and libuv
   set flags with read-modify-write. Pinned by Task 2 check 15.
2. **SIGIO interrupting a blocking call:** a handler installed without
   `SA_RESTART` interrupts a blocked `poll` with `EINTR`; with
   `SA_RESTART` a blocked `read` resumes. Pinned by Task 2 check 23.
3. **Unhandled SIGIO uses Linux's default (terminate)**, not QNX's. Pinned
   by Task 2 check 21.
4. **Sockets from `socket()`+`accept()` (TCP loopback), not only
   `socketpair`.** Pinned by Task 2 check 22.
5. **Repeated reloads:** nginx survives two `-s reload` in a row and still
   serves. Pinned by Task 3 step 1.

---

### Task 1: Probe QNX socket async I/O

**Files:**
- Create: `native-probes/phase3d-sigio.c`
- Modify: `scripts/build-native-probes.sh` (build it, `-lsocket`)
- Modify: `docs/FINDINGS.md` (new section "QNX socket async I/O")

**Interfaces:**
- Produces: the **allow-list** of socket kinds (family × type) for which
  QNX delivers SIGIO, and recorded answers to each question below, which
  Task 2 implements against.

- [ ] **Step 1: Write the probe.** Native QNX C, one line per result,
  `key=value`, a 3 s `alarm` per case so a hang is reported, not suffered.
  For each of `AF_UNIX SOCK_STREAM` (socketpair), `AF_UNIX SOCK_DGRAM`
  (socketpair), TCP loopback (connected pair via `listen`/`connect`/
  `accept`), a TCP listening socket (a pending `connect` → readable), UDP
  loopback, a pty master, and a pipe, record:
  `FIOASYNC` result; `F_SETOWN(getpid())` result; `F_GETOWN` value and
  whether it equals `getpid()`; `F_GETFL` QNX `O_ASYNC` bit after
  `FIOASYNC`; whether a peer write (or connect, for listening) delivers
  SIGIO and `si_signo`/`si_code`/`si_fd`. Also, on `AF_UNIX SOCK_STREAM`:
  `F_SETFL O_ASYNC` alone (no `FIOASYNC`) then a peer write; SIGIO on peer
  close; `F_SETOWN(-getpgrp())` then a peer write; owner and flag in a
  `fork` child (and whether the child's peer write signals the parent);
  owner and flag after `execv` of the probe itself with a mode argument;
  which thread runs the handler when another thread is blocked in `read`.

- [ ] **Step 2: Build and run on the device; save the output.**
  Run: `sh scripts/build-native-probes.sh && build/native-probes/phase3d-sigio`
  Expected: every case prints a line; no case hits its alarm except a
  case QNX really hangs on (then recorded as a hang).

- [ ] **Step 3: Record in `docs/FINDINGS.md`:** a table of the cases, the
  allow-list, and the answers (owner form, process-group support, fork,
  exec, peer close, thread), dated 2026-11-20, without any IP address.

- [ ] **Step 4: Commit** ("Probe QNX socket async I/O for Phase 3 part 4").

### Task 2: Socket async I/O

**Files:**
- Create: `guest-tests/async-io.c`, `guest-tests/fixtures/async-io-armhf`
- Modify: `src/linux_socket.c` (predicate), `src/linuxemu.h` (prototype),
  `src/linux_termios.c` (`FIOASYNC`), `src/linux_syscall.c` (`fcntl`
  cases 3, 4, 8, 9), `src/guest_signal.c` (only if check 21 needs it),
  `scripts/run-event-smoke.sh` (`run_rootfs_fixture async-io`)

**Interfaces:**
- Consumes: Task 1's allow-list and answers.
- Produces: `int linux_socket_async_capable(int fd)` — 1 when `fd` is a
  host socket with no typed object and its kind is on the allow-list,
  else 0 (errno untouched).

- [ ] **Step 1: Save the pre-phase core** on the device:
  `cp build/linuxemu build/linuxemu-pre-phase3d`.

- [ ] **Step 2: Write the fixture `guest-tests/async-io.c`.** Checks (an
  allow-listed socket is `socketpair(AF_UNIX, SOCK_STREAM)` unless stated;
  where Task 1 recorded QNX behaviour, assert that behaviour):
  - 10: `ioctl(s, FIOASYNC, &1) == 0`, and `fcntl(s, F_GETFL) & 0x2000`.
  - 11: `fcntl(s, F_SETOWN, getpid()) == 0` and `fcntl(s, F_GETOWN) == getpid()`.
  - 12: a peer write delivers SIGIO to an `SA_SIGINFO` handler with
    `si_signo == 29`.
  - 13: `ioctl(s, FIOASYNC, &0)` clears `0x2000` from `F_GETFL`, and a peer
    write is not delivered.
  - 14: `fcntl(s, F_SETFL, F_GETFL | 0x2000)` alone enables delivery.
  - 15: `F_SETFL` with `O_ASYNC|O_NONBLOCK` sets both; then `F_SETFL` with
    only `O_ASYNC` clears `O_NONBLOCK` and keeps `O_ASYNC` (Review Focus 1).
  - 16: pipe: `FIOASYNC` → `-1/EINVAL`; `F_SETFL O_ASYNC` → `-1/EINVAL`
    with `F_GETFL` unchanged; `F_SETOWN` → `-1/EINVAL`.
  - 17: eventfd and a regular file: `FIOASYNC` → `-1/EINVAL`.
  - 18: `ioctl(s, FIOASYNC, (int *)1)` → `-1/EFAULT`.
  - 19: `fork` child: `O_ASYNC` and owner as Task 1 recorded.
  - 20: re-exec self with a mode argument: `O_ASYNC` and owner as recorded.
  - 21: a `fork` child that enables async I/O with SIGIO at its default
    disposition is killed by a peer write: `WIFSIGNALED && WTERMSIG == 29`
    (Review Focus 3).
  - 22: TCP loopback (`socket`/`bind 127.0.0.1:0`/`listen`/`connect`/
    `accept`): async on the accepted socket, a peer write delivers SIGIO
    (Review Focus 4). If Task 1 left a socket kind off the allow-list, also
    assert that kind gives `FIOASYNC -1/EINVAL`.
  - 23: handler without `SA_RESTART` interrupts a blocked `poll` (no events,
    5 s timeout) with `EINTR` when a peer write from a helper thread
    signals; with `SA_RESTART` a blocked `read` returns the written byte
    (Review Focus 2).
  - 24: `F_SETOWN(-getpgrp())`: delivered, or `-1/EINVAL` if Task 1 found
    QNX can't do process groups.

- [ ] **Step 3: Build the fixture and run it on the pre-phase core.**
  Run: `LINUXEMU=build/linuxemu-pre-phase3d sh scripts/run-event-smoke.sh`
  Expected: `async-io` fails (exit 10: `FIOASYNC` gives `ENOTTY`).

- [ ] **Step 4: Implement.**
  - `linux_socket_async_capable` in `src/linux_socket.c` from `fstat`,
    `guest_fd` typed-object lookup, `getsockopt(SO_TYPE)` and the address
    family (`getsockname`).
  - `FIOASYNC` in `src/linux_termios.c`: read the guest `int` (`EFAULT` on
    a bad pointer), forward to QNX `FIOASYNC` when capable, else `EINVAL`
    via `linux_report_unsupported(54, EINVAL, "async I/O (FIOASYNC) is only supported on sockets", args)`.
  - `fcntl` `F_GETFL`: map QNX `O_ASYNC` to 0x2000. `F_SETFL`: accept
    0x2000; capable → include QNX `O_ASYNC` in the flags set, together with
    `O_APPEND`/`O_NONBLOCK`; not capable and 0x2000 requested →
    `linux_report_unsupported(221, EINVAL, "async I/O (O_ASYNC) is only supported on sockets", args)`
    and no change.
  - `F_SETOWN`/`F_GETOWN`: capable → forward (translate or refuse
    negative owners per Task 1); else keep the `fcntl-owner` report, detail
    changed to `"fcntl-owner: only sockets support F_SETOWN"`.
  - Linux SIGIO default action must terminate (check 21); change
    `src/guest_signal.c` only if the check fails.

- [ ] **Step 5: Run on the new core.**
  Run: `sh scripts/build-native-probes.sh && sh scripts/run-event-smoke.sh`
  Expected: `linuxemu_event_smoke_failures=0`, including `async-io exit=0`.

- [ ] **Step 6: Commit** ("Pass socket async I/O through to QNX").

### Task 3: nginx serves, times out, reloads and stops

**Files:**
- Modify: `scripts/corpus/apps/nginx.sh`
- Modify: any `src/` file a Phase 1–3-kind gap requires (see Global
  Constraints, "needs the user's OK")

**Interfaces:**
- Consumes: Task 2's async I/O.

- [ ] **Step 1: Add the checks.** In `nginx.conf` add
  `client_header_timeout 2s;`. In `stage_basic`, after the GET: a guest
  client opens a TCP connection to 127.0.0.1:18081, sends nothing, and the
  stage asserts nginx closes it within 2–6 s (read returns 0 / connection
  reset), measured in the guest. In `stage_concurrent`, after the parallel
  GETs: record the worker PID, run `nginx -s reload` **twice** (wait for
  each to settle, ≤10 s), then assert a GET of `index.html` returns `ok`
  and the worker PID differs from the first. Failure messages name the
  check ("idle client not closed", "worker not replaced after reload").

- [ ] **Step 2: Run nginx in the corpus on the new core.**
  Run: `ssh -tt … 'cd DEV && sh scripts/run-corpus.sh nginx'`
  Expected: `nginx install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes`.

- [ ] **Step 3: If a stage fails,** diagnose with `LINUXEMU_DIAGNOSTICS`
  and the error log; fix Phase 1–3-kind gaps (each fix with a focused
  fixture check or a corpus check that fails before and passes after),
  otherwise add to the needs-OK list. Repeat step 2.

- [ ] **Step 4: Commit** ("Make nginx serve, time out, reload and stop").

### Task 4: Build and run libuv's tests

**Files:**
- Create: `scripts/fetch-libuv-source.sh` (laptop), `scripts/build-libuv-tests.sh`,
  `scripts/run-libuv-smoke.sh`, `scripts/libuv-subset.txt`
- Modify: `scripts/run-full-regression.sh` (`run_suite run-libuv-smoke`
  after `run-event-smoke`), `docs/development.md` (how to fetch, build, run)

**Interfaces:**
- Produces: `build/libuv/uv_run_tests` (guest binary path
  `/tmp/linuxemu-libuv/uv_run_tests` inside the rootfs while running);
  `run-libuv-smoke.sh` printing one `NAME exit=N expected=0` line per test
  and a final `linuxemu_libuv_failures=N`; `run-libuv-smoke.sh --all`
  runs every test in the suite and prints `libuv_all pass=P fail=F skip=S`.

- [ ] **Step 1: Pin the version.** Find the libuv version Alpine 3.24
  ships (the package index the corpus fetch uses). `fetch-libuv-source.sh`
  downloads `https://dist.libuv.org/dist/v<VER>/libuv-v<VER>.tar.gz` into
  `artifacts/libuv/` (git-ignored), checks a pinned sha256 written in the
  script, and extracts it. No network access is needed on the device.

- [ ] **Step 2: Build script.** `build-libuv-tests.sh` copies the source
  into the rootfs, installs the build tools from the existing pinned
  `docs/corpus/gcc-make-apks.sha256` closure (reuse `scripts/corpus/lib.sh`'s
  pinned install; this satisfies the spec's "pinned closure" for gcc,
  make, musl-dev), compiles libuv's `src/` (unix + linux files from the
  upstream `CMakeLists.txt` lists) and `test/` into one static
  `uv_run_tests` with the guest gcc (`-O1 -pthread -D_GNU_SOURCE`), copies
  it to `build/libuv/`, and removes the tools and sources, restoring the
  package world. A rebuild is skipped when `build/libuv/uv_run_tests` is
  newer than the source.

- [ ] **Step 3: Subset list.** `scripts/libuv-subset.txt`: one test name
  per line, from that version's `test/test-list.h`, covering the spec's
  families: `timer_*`, `idle_*`, `loop_*` (alive/stop/update_time/close),
  `ref`/`unref` basics, `async*`, `poll_duplex`, `poll_unidirectional`,
  `poll_close`, `threadpool_*`, `signal_*` and `we_get_signal*`, `spawn_*`
  that use only pipes and exit codes, `pipe_*` on a path under `/tmp`,
  TCP on 127.0.0.1 (`tcp_ping_pong`, `tcp4_echo`-style, `tcp_close*`,
  `tcp_shutdown*`), and `fs_event_*`. Exclude IPv6, TTY, DNS, `/proc`
  and process-title tests by construction. Comment each family.

- [ ] **Step 4: Runner.** `run-libuv-smoke.sh` runs each listed test as
  `uv_run_tests NAME NAME` from `/tmp/linuxemu-libuv` in the guest, with a
  per-test timeout (default 60 s; ended → exit 124), and counts failures.

- [ ] **Step 5: Run the subset and record the starting point** in the task
  report (pass/fail per test, with the first failure line). Do not fix
  anything in this task.
  Run: `ssh -tt … 'cd DEV && sh scripts/build-libuv-tests.sh && sh scripts/run-libuv-smoke.sh'`
  Expected: the suite runs to completion and prints `linuxemu_libuv_failures=N`
  (N may be > 0 here).

- [ ] **Step 6: Commit** ("Build and run libuv's event-loop tests").

### Task 5: Make the libuv subset pass

**Files:**
- Modify: whichever `src/` files the failures require; new fixture checks
  in the existing `guest-tests/*.c` that own the area (or a new fixture
  `guest-tests/libuv-gaps.c` added to `run-event-smoke.sh`)
- Modify: `docs/superpowers/plans/2026-11-20-phase3d-needs-ok.md` when needed

**Interfaces:**
- Consumes: Task 4's runner and starting point.

- [ ] **Step 1: For each failing subset test,** find the cause
  (`LINUXEMU_DIAGNOSTICS`, the test's output, a minimal guest repro).
  Phase 1–3-kind gap → write a fixture check that reproduces it and fails
  on the current core; otherwise → needs-OK list, and leave the test in
  the subset.

- [ ] **Step 2: Fix each reproduced gap**; the fixture check passes and the
  libuv test passes.

- [ ] **Step 3: Run the subset and the event smoke.**
  Run: `sh scripts/run-event-smoke.sh && sh scripts/run-libuv-smoke.sh`
  Expected: `linuxemu_event_smoke_failures=0`; `linuxemu_libuv_failures`
  equals the number of tests on the needs-OK list (0 if the list is empty).

- [ ] **Step 4: Commit** each fix separately ("Fix <gap> found by libuv
  <test>").

### Task 6: Gate and docs

**Files:**
- Modify: `docs/compatibility.md` ("Limits of the Phase 3 part 4 calls"),
  `docs/FINDINGS.md`, `docs/corpus-results.md` (new section with the corpus
  run and the full libuv suite results), `docs/capability-roadmap.md`
  (Phase 3 part 4 status; Phase 3 overall stays open for Node.js unless the
  user moves it), `docs/development.md`, `README.md`

- [ ] **Step 1: Full corpus.** Run: `ssh -tt … 'cd DEV && sh scripts/run-corpus.sh'`
  Expected: `corpus apps=11 compatible=10` (Node.js still failing), every
  previously compatible app still compatible, nginx compatible.

- [ ] **Step 2: Full libuv suite once.** Run: `sh scripts/run-libuv-smoke.sh --all`
  Expected: completes; record `libuv_all pass= fail= skip=` and each
  failing test's one-line cause in `docs/corpus-results.md`.

- [ ] **Step 3: Full regression.** Run: `ssh -tt … 'cd DEV && sh scripts/run-full-regression.sh'`
  Expected: `linuxemu_full_regression_failures=0` with `run-libuv-smoke=ok`
  (if the subset still has needs-OK tests, the gate is not met: report it).

- [ ] **Step 4: Docs.** Limits: async I/O only on the allow-listed socket
  kinds; no `F_SETSIG`/`F_*OWN_EX`/`SIGURG`; `si_code`/`si_fd` as Task 1
  found; process-group owners as found; anything Tasks 3–5 documented.
  README: nginx works. Roadmap: part 4 status with gate results, Phase 3
  open items (Node.js only, plus any needs-OK items). No IP addresses.

- [ ] **Step 5: Commit** ("Record the Phase 3 part 4 gate").
