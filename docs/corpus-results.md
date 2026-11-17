# Application corpus results

Produced by `sh scripts/run-corpus.sh` (see the README, "Application corpus").
Each app is scored on five stages: install, startup, basic workload, concurrent
workload, and clean shutdown. An app is compatible only when all five pass,
because installation alone doesn't count. `unsupported` is the number of
distinct unsupported calls in the app's `LINUXEMU_DIAGNOSTICS` report.

## Baseline: 2026-11-17

- **Device:** Passport, QNX 8.0.0, as the ordinary user (devuser).
- **Linuxemu:** `master` at `d3418b3`. The corpus branch changes scripts only.
- **Rootfs:** Alpine 3.24.2 armhf test rootfs. Each app ran in a fresh copy.
  Afterwards the base still had 16 packages and `musl-1.2.6-r2`.
- **Packages:** pinned closures listed in `docs/corpus/*-apks.sha256`.

```text
apk install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=2
busybox install=pass startup=pass basic=pass concurrent=pass shutdown=fail compatible=no unsupported=3
gcc-make install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=5
git install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=2
inotify-tools install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=4
nginx install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=4
node install=pass startup=fail basic=skip concurrent=skip shutdown=skip compatible=no unsupported=2
python install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=7
redis install=pass startup=pass basic=fail concurrent=skip shutdown=skip compatible=no unsupported=4
sqlite install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=2
vim install=pass startup=pass basic=pass concurrent=pass shutdown=pass compatible=yes unsupported=4
corpus apps=11 compatible=6
```

**After the baseline, BusyBox was rerun with the deadlock fix (see below):**
all five stages pass, compatible=yes. That makes the corpus **7 of 11
compatible**.

### Why each failing app fails

| App | Stage | Cause | Roadmap |
|---|---|---|---|
| nginx | basic | `epoll_create`/`epoll_create1` return ENOSYS, so nginx never serves | Phase 3 (epoll) |
| Redis | basic | `epoll_create`/`epoll_create1` return ENOSYS, so the server never answers PING | Phase 3 (epoll) |
| inotify-tools | basic | `inotify_init`/`inotify_init1` return ENOSYS ("Couldn't initialize inotify") | Phase 3 (inotify) |
| Node.js | startup | Node's `ResetSignalHandlers` asserts on `sigaction(n, NULL, &old)`. Linuxemu returns EINVAL for Linux signals with no QNX mapping, while Linux allows the query for every signal from 1 to 64. | Signal ABI (Phase 2/6) |
| BusyBox | shutdown | **Fixed.** With `LINUXEMU_DIAGNOSTICS` set, SIGTERM to a fresh fork child ran the diagnostics terminate handler, which called `getenv` and `pthread_kill`. Those aren't signal-safe, so the child deadlocked on a libc MUTEX during exec set-up (2 in 10 corpus-style launches). The handler now uses only signal-safe calls: smoke `diag-sigterm-race` went from 2/20 to 0/20 | Linuxemu diagnostics (Phase 0A) |

### Unsupported calls, total occurrences across the run

| Call | Apps (occurrences) |
|---|---|
| `statx` (397) | all 11; Python 1720, Git 735, GCC 631, Vim 411, SQLite 340, Redis 189 |
| `pwritev2` (393) | SQLite 110, Python 14 |
| `setitimer` (104) | Python 96 |
| `getrusage` (77) | GCC 80 |
| `sysinfo` (116) | GCC 18, Vim 12, Python 4, Redis 1 |
| `sendfile64` (239) | BusyBox 10, Vim 5, inotify-tools 1 |
| `fchmod` (94) | Vim 5 |
| `chown32` (212) | BusyBox 4 |
| `faccessat2` (439) | GCC 6 |
| `epoll_create1` (357) / `epoll_create` (250) | nginx, Redis, Python 3 |
| `inotify_init` (316) / `inotify_init1` (360) | inotify-tools |
| `sched_getaffinity` (242) | Git 2, nginx 1 |
| `membarrier` (389) | Python 2 |
| `mremap` (163), `mknod` (14), `statfs64` (266), `capget` (184) | 1 each (apk, GCC, Python, Node) |

The compatible apps also hit unsupported calls. They tolerate them by falling
back, for example from `statx` to `fstatat`, so these are still worth
implementing.
