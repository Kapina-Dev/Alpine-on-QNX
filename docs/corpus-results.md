# Application corpus results

Produced by `sh scripts/run-corpus.sh` (see `docs/development.md`, "Application corpus").
Each app is scored on five stages: install, startup, basic workload, concurrent
workload, and clean shutdown. An app is compatible only when all five pass,
because installation alone doesn't count. `unsupported` is the number of
distinct unsupported calls in the app's `LINUXEMU_DIAGNOSTICS` report.

## Baseline: 2026-11-17

- **Device:** Passport, QNX 8.0.0, as the ordinary user (devuser).
- **Linuxemu:** the `phase0b-corpus` branch: `master` at `d3418b3` plus the
  diagnostics signal-handler deadlock fix described below. The device build
  also carried a local, uncommitted trace-only edit to `src/linux_socket.c`
  (its `fprintf` calls run only with runtime tracing on, which these runs
  don't enable).
- **Harness:** the version after the final review. Server stages refuse to
  start when their port already answers, and a timed-out or interrupted
  stage kills the guests it started.
- **Rootfs:** Alpine 3.24.2 armhf test rootfs. Each app ran in a fresh copy.
  Afterwards the base still had 16 packages and `musl-1.2.6-r2`.
- **Packages:** pinned closures listed in `docs/corpus/*-apks.sha256`.
- **Leftovers:** no Linuxemu process was left running after the run.

```text
apk install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=2
busybox install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=3
gcc-make install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=5
git install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=2
inotify-tools install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=4
nginx install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=5
node install=pass startup=fail basic=skip concurrent=skip shutdown=skip compatible=no unsupported=2
python install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=7
redis install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=4
sqlite install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=2
vim install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=4
corpus apps=11 compatible=7
```

### Why each failing app fails

| App | Stage | Cause | Roadmap |
|---|---|---|---|
| nginx | basic | `epoll_create`/`epoll_create1` return ENOSYS, so nginx never serves ("server did not answer within 60s") | Phase 3 (epoll) |
| Redis | basic | `epoll_create`/`epoll_create1` return ENOSYS, so the server exits at start-up ("server exited (status 1) before answering") | Phase 3 (epoll) |
| inotify-tools | basic | `inotify_init`/`inotify_init1` return ENOSYS ("Couldn't initialize inotify") | Phase 3 (inotify) |
| Node.js | startup | Node's `ResetSignalHandlers` asserts on `sigaction(n, NULL, &old)`. Linuxemu returns EINVAL for Linux signals with no QNX mapping, while Linux allows the query for every signal from 1 to 64. | Signal ABI (Phase 2/6) |

BusyBox failed shutdown in the first baseline run. The cause was a Linuxemu
bug, now fixed on this branch: with `LINUXEMU_DIAGNOSTICS` set, SIGTERM to a
fresh fork child ran the diagnostics terminate handler, which called
`getenv` and `pthread_kill`. Those aren't signal-safe, so the child
deadlocked on a libc MUTEX during exec set-up (2 in 10 corpus-style
launches). The handler now uses only signal-safe calls, and smoke
`diag-sigterm-race` went from 2/20 to 0/20.

### Unsupported calls per app

These are the distinct unsupported calls in each app's diagnostics report,
with how many times each was hit during the run, most frequent first. The
counts include helper guests started with the same report (BusyBox `wget`
and `sh`, `redis-cli`). Every call returned ENOSYS.

| App | Calls (occurrences) |
|---|---|
| apk | `statx` 64, `mremap` 1 |
| BusyBox | `statx` 46, `sendfile64` 10, `chown32` 4 |
| GCC/make | `statx` 631, `getrusage` 80, `sysinfo` 18, `faccessat2` 6, `mknod` 1 |
| Git | `statx` 735, `sched_getaffinity` 2 |
| inotify-tools | `statx` 20, `inotify_init` 1, `inotify_init1` 1, `sendfile64` 1 |
| nginx | `statx` 5, `sched_getaffinity` 1, `epoll_create` 1, `epoll_create1` 1, `setitimer` 1 |
| Node.js | `statx` 21, `capget` 1 |
| Python | `statx` 1726, `setitimer` 97, `pwritev2` 14, `sysinfo` 4, `epoll_create1` 3, `membarrier` 2, `statfs64` 1 |
| Redis | `statx` 12, `epoll_create` 1, `sysinfo` 1, `epoll_create1` 1 |
| SQLite | `statx` 332, `pwritev2` 102 |
| Vim | `statx` 411, `sysinfo` 12, `fchmod` 5, `sendfile64` 5 |

The compatible apps also hit unsupported calls. They tolerate them by falling
back, for example from `statx` to `fstatat`, so these are still worth
implementing.

### Linuxemu divergence found by the review: inherited SIG_IGN

A background job of a non-interactive shell starts with SIGINT and SIGQUIT
ignored. On Linux that disposition survives `execve`, and `sigaction`
reports it as SIG_IGN. Linuxemu starts every guest with all Linux signal
actions recorded as default, and it doesn't read the inherited host
disposition. As a result, a query reports SIG_DFL for a signal that is
really ignored. A program that installs its handler only when a signal is at
default, as CPython does for SIGINT, then takes over a signal that Linux
would leave ignored. With `LINUXEMU_DIAGNOSTICS` set, the diagnostics
terminate handler also replaces the inherited SIG_IGN. The Python shutdown
stage used to pass only because of this. It now installs Python's SIGINT handler itself,
which makes the stage hold under Linux semantics too. The divergence itself
is still open (signal ABI work, Phase 2/6).

## Phase 2: 2026-11-18

- **Device:** Passport, QNX 8.0.0, as the ordinary user.
- **Linuxemu:** branch `phase2-syscalls` at `4132ccd`, core sha256
  `9ec53575afe18e656ef04244d46d5cf79437a5c7599e33000e95f39c06b2609b`.
- **Rootfs, packages, harness:** unchanged from the 2026-11-17 baseline.
- **Leftovers:** no Linuxemu process was left running.

```text
apk install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
busybox install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=1
gcc-make install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
git install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
inotify-tools install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=2
nginx install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=2
node install=pass startup=fail basic=skip concurrent=skip shutdown=skip compatible=no unsupported=1
python install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=3
redis install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=2
sqlite install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=1
vim install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
corpus apps=11 compatible=7
```

The score is the same as the baseline (7 of 11) and every stage result is
unchanged. The unsupported-call counts dropped for every app: the baseline
totals 2 to 7 per app, and now they are 0 to 3.

### Unsupported calls per app

None of the calls Phase 2 implemented returns ENOSYS any more. `statx`,
`getrusage`, `sysinfo`, `sched_getaffinity`, `capget`, `faccessat2`, `mknod`,
`chown32`, `fchmod`, `statfs64`, `setitimer` and `mremap` no longer appear.
What remains is either Phase 3 work or a deliberate refusal.

| App | Calls (occurrences) |
|---|---|
| apk | none |
| BusyBox | `sendfile64` 3 (EINVAL, `sendfile-input`) |
| GCC/make | none |
| Git | none |
| inotify-tools | `inotify_init` 1, `inotify_init1` 1 (ENOSYS) |
| nginx | `epoll_create` 1, `epoll_create1` 1 (ENOSYS) |
| Node.js | `epoll_create1` 1 (ENOSYS) |
| Python | `epoll_create1` 3 (ENOSYS), `pwritev2` 14 (EOPNOTSUPP, `rwf-flags`), `membarrier` 2 (EINVAL, `membarrier-cmd`) |
| Redis | `epoll_create` 1, `epoll_create1` 1 (ENOSYS) |
| SQLite | `pwritev2` 102 (EOPNOTSUPP, `rwf-flags`) |
| Vim | none |

- `pwritev2` is implemented. SQLite and Python pass `RWF_NOAPPEND` (flags
  `0x20`, from musl's `pwrite`). This run refused it with `EOPNOTSUPP` and
  musl fell back to plain `pwrite`. Linuxemu now accepts `RWF_NOAPPEND`, so
  these reports are gone from later runs.
- `membarrier` is reported on purpose: Linuxemu supports no commands, so
  musl uses its signal-based fallback.
- BusyBox `tar` makes a `sendfile64` call whose input descriptor Linuxemu
  refuses. `tar` falls back to read and write.

### Node.js

| Stage | Result |
| --- | --- |
| install | pass |
| startup | fail |
| basic, concurrent, shutdown | skipped after startup failed |

The `ResetSignalHandlers` abort is gone. The fix came from the
`rt_sigaction` change (Task 1): signals with no QNX number can now be
queried and stored. Node starts, then dies on an assertion in
`node::tracing::LegacyTracingAgent::LegacyTracingAgent()`
(`../../src/tracing/agent_legacy.cc:88`). The only unsupported call in its
report is `epoll_create1` with `EPOLL_CLOEXEC` (ENOSYS, once), and libuv
can't create its loop without it. Node's next blocker is therefore `epoll`,
which is Phase 3 work. `statx` and `capget` no longer appear, and
`cacheflush` is not reached, so V8's JIT hasn't run yet.

### Phase gate notes

- The full regression passes every suite up to `run-python-smoke`. That
  suite fails on this core and on the pre-phase core
  (`build/linuxemu-pre-phase2`, sha256 `5089cc4c...`) with the same fault: a
  python3 SIGSEGV at pc `0x60067dc4`, address 0 (exit 125). It failed in 4 of 4
  runs on 2026-11-18 on the new core, and once on the pre-phase core. It
  passed earlier in the phase (Task 6). It is a pre-existing fault in the
  smoke test's own Python workload, not a pass, and it is not caused by
  Phase 2. The corpus Python app passes all five stages.
- `run-git-smoke`, which the full regression runs after `run-python-smoke`,
  passed when run separately on the same core (rc=0).

## Phase 3 part 1: 2026-11-19

- **Device:** Passport, QNX 8.0.0, as the ordinary user.
- **Linuxemu:** branch `phase3a-events` at `b38f3cb` (core unchanged since
  `1e3b521`), core sha256
  `fd0537628f93b998a63a5fa8059b6baddd285b5e8b8cf2b5e0f11a76ecc8f647`.
- **Rootfs, packages, harness:** unchanged from the 2026-11-17 baseline.
- **Leftovers:** no Linuxemu process was left running; the harness killed
  the nginx server it started (status 137).

```text
apk install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
busybox install=pass startup=pass basic=pass concurrent=pass shutdown=fail compatible=no unsupported=1
gcc-make install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
git install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
inotify-tools install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=2
nginx install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=0
node install=pass startup=fail basic=skip concurrent=skip shutdown=skip compatible=no unsupported=3
python install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=1
redis install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=1
sqlite install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
vim install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=0
corpus apps=11 compatible=7
```

The score is 7 of 11 again, but the set changed. **Redis is now compatible**
(all five stages pass on epoll; `redis-benchmark` ran about 600 SET and
GET requests per second). BusyBox failed its shutdown stage once (see
below) and passed it in three reruns; with it the score would be 8 of 11.

### Why each failing app fails

| App | Stage | Cause | Roadmap |
|---|---|---|---|
| nginx | basic | The master's `ioctl(channel, FIOASYNC)` (Linux `0x5452`) on the master/worker socketpair returns `ENOTTY`, so nginx logs `ioctl(FIOASYNC) failed while spawning "worker process" (25: Not a tty)` and runs with no worker; nothing accepts ("server did not answer within 60s"). Epoll itself works: the error log shows `using the "epoll" event method`. Linuxemu returns `ENOTTY` for unknown ioctls without a diagnostic, so the report lists nothing. | Unscheduled (ioctl coverage) |
| Node.js | startup | Past `epoll_create1`. Node now dies with a fatal SIGSEGV (`linuxemu: fatal signal=0x0000000b pc=0x0116d688 address=0x00000000`) before printing its version. Its report shows `madvise` (ENOSYS, 17 calls) and `mremap` refused (`EINVAL`, `mremap-kind`) just before; not yet diagnosed further. | Phase 4/6 (V8 memory and JIT) |
| inotify-tools | basic | `inotify_init`/`inotify_init1` return ENOSYS ("Couldn't initialize inotify") | Phase 3 part 3 |
| BusyBox | shutdown | Intermittent: once, `sleep 30 &; kill -TERM; wait` returned 143 but `kill -0` still found the pid ("still-alive"). Three reruns of the BusyBox app passed, and the same check passed 60 of 60 times on this core and on the pre-phase core without `LINUXEMU_DIAGNOSTICS`. A watch item, like the 2026-11-17 diagnostics SIGTERM race. | Watch |

nginx was reproduced by hand with a debug error log (`build/t9/nginx-probe.sh`
on the device, not committed) to find the `FIOASYNC` call.

### Unsupported calls per app

| App | Calls (occurrences) |
|---|---|
| apk, GCC/make, Git, nginx, SQLite, Vim | none |
| BusyBox | `sendfile64` 3 (EINVAL, `sendfile-input`) |
| inotify-tools | `inotify_init` 1, `inotify_init1` 1 (ENOSYS) |
| Node.js | `madvise` 17 (ENOSYS), `mremap` 1 (EINVAL, `mremap-kind`), `membarrier` 1 (EINVAL, `membarrier-cmd`) |
| Python | `membarrier` 2 (EINVAL, `membarrier-cmd`) |
| Redis | `membarrier` 1 (EINVAL, `membarrier-cmd`) |

No `epoll_*` or `eventfd*` call is reported any more. Python's
`epoll_create1` ENOSYS and the `pwritev2` refusals of the Phase 2 run are
gone.
