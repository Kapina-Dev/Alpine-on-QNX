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

## Phase 2

Progress note after Task 2 (ARM `cacheflush` and the `rt_sigaction` fix).
The full Phase 2 baseline comes at the end of the phase.

### Node.js

| Stage | Result |
| --- | --- |
| install | pass |
| startup | fail |
| basic, concurrent, shutdown | skipped after startup failed |

The `ResetSignalHandlers` abort is gone. Node now starts, then dies on an
assertion in `node::tracing::LegacyTracingAgent::LegacyTracingAgent()`
(`../../src/tracing/agent_legacy.cc:88`). The diagnostics report shows the
last unsupported call before that abort is `epoll_create1` (357, flags
`0x80000` = `EPOLL_CLOEXEC`, ENOSYS, once). Node's libuv loop can't be
created without it, which fits the assertion. The same run also reports
`statx` (27 calls) and `capget` (11 calls) as ENOSYS. `cacheflush` doesn't
appear in the report, so V8's JIT was not reached. No fix was attempted;
`epoll` is Phase 3 work.
