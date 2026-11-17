# Phase 0B: Pinned Application Corpus Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a repeatable corpus runner that installs each pinned
application into the Alpine test rootfs under Linuxemu. It scores five stages
per app (install, startup, basic workload, concurrent workload, clean
shutdown) and records the unsupported-syscall diagnostics for each app.

**Architecture:**
- POSIX `sh`, like the existing smoke scripts.
- A small harness library (`scripts/corpus/lib.sh`) runs the five stages of
  one app definition. It skips later stages after a failure, always runs
  cleanup, and checks that the rootfs package world is restored.
- Each app is one definition file (`scripts/corpus/apps/<app>.sh`) providing
  five stage functions.
- Packages are pinned like the existing Python and Git suites: an offline
  `.apk` closure under the git-ignored `artifacts/corpus/<app>/`, with a
  committed sha256 manifest.
- The corpus is a measurement, separate from the release regression. A
  failing app stage is a result, not a test failure.

**Tech Stack:**
- POSIX `sh` scripts, run on the Passport.
- Alpine 3.24.2 armhf test rootfs, `apk` under Linuxemu.
- `LINUXEMU_DIAGNOSTICS` reports from Phase 0A.

**Spec:** `docs/capability-roadmap.md`, "Phase 0: measurable compatibility",
the paragraph "Create a pinned application corpus …" and its five scoring
items, plus "Installation alone must not count as application compatibility."

## Global Constraints

- Corpus apps, from the spec: "BusyBox, apk, Python, Git, SQLite, nginx,
  Node.js/libuv, Redis, Vim, GCC/make, and selected file-watching tools".
  File-watching tool: `inotify-tools` (`inotifywait`).
- Score each app separately for "1. package installation; 2. executable
  startup; 3. basic workload completion; 4. concurrent workload completion;
  and 5. clean shutdown and resource cleanup."
- "Installation alone must not count as application compatibility." An app
  counts as compatible only when all five stages pass.
- "Preserve the current release regression": the corpus is **not** added to
  `scripts/run-full-regression.sh`, and the regression must stay at
  `linuxemu_full_regression_failures=0`.
- "Return correct Linux errors … do not use success stubs": no stage may be
  forced to pass. A stage that can't run is `skip`, with a reason.
- Pinned to Alpine 3.24 armhf. The rootfs is restored after every app:
  `etc/apk/world` must match its pre-run copy.
- Ordinary-user execution only.

## Result format (decided here; Phase 8 releases read it)

`build/corpus/results.tsv` has one line per app per stage, with tab-separated
fields:

```
<app>	<stage>	<pass|fail|skip>	<seconds>	<note>
```

- Stage names are `install`, `startup`, `basic`, `concurrent` and `shutdown`.
- The note has no tabs or newlines. For `fail` it is the first line of the
  failing command's output, cut to 120 characters. For `skip` it is
  `after <stage> failed`.

`build/corpus/summary.txt` has one line per app, then one totals line:

```
<app> install=<r> startup=<r> basic=<r> concurrent=<r> shutdown=<r> compatible=<yes|no> unsupported=<n>
corpus apps=<n> compatible=<n>
```

`unsupported` counts the distinct `linuxemu-unsupported` lines in
`build/corpus/<app>/diagnostics.txt`. Each app's stage logs and diagnostics
live in `build/corpus/<app>/`.

## Review Focus

- **Cleanup when a stage fails or hangs:** the app's packages are removed, its
  background processes are killed and `etc/apk/world` is restored, even after
  a timeout. Pinned in Task 1 (`fail-cleanup` and `hang-cleanup` harness
  tests).
- **A previous run left the app installed:** the runner refuses to score that
  app (recorded as `install fail`, with note `rootfs not clean`, because its
  virtual package is still installed) instead of reporting a false install
  pass. Pinned in Task 1 (`dirty-rootfs` test).
- **Tampered or missing pinned archive:** install fails and Linuxemu doesn't
  run. Pinned in Task 2 (`tampered-archive` check).
- **Port or file left behind by a server app:** the shutdown stage must fail
  if the port still accepts connections or the pid file remains. Pinned in
  Task 4 (nginx and redis shutdown stages).
- **One app's crash stops the whole corpus:** the runner always moves on to
  the next app. Pinned in Task 1 (`continues-after-crash` test).

---

### Task 1: Harness library and runner

**Files:**
- Create `scripts/corpus/lib.sh`. It is sourced, not executed.
- Create `scripts/run-corpus.sh`. Usage: `sh scripts/run-corpus.sh [app ...]`.
  With no arguments it runs every file in `scripts/corpus/apps/` in sorted
  order. With arguments it runs only those apps.
- Test: `tests/corpus-harness-test.sh`. It runs on the **host**: it sets
  `CORPUS_APPS_DIR` to a temporary directory of fake app definitions and
  `CORPUS_FAKE=1`. In fake mode the harness doesn't require Linuxemu or the
  rootfs, and `world` is a temporary file.

**Interfaces:**
- Produces, for app definitions:
  - Variables an app sets: `corpus_packages` (space-separated apk names, used
    by Task 2) and `corpus_virtual` (default `.linuxemu-corpus-<app>`).
  - Functions an app defines:
    - `stage_startup`, `stage_basic`, `stage_concurrent` and `stage_shutdown`;
    - optional `stage_install`, whose default installs the pinned closure;
    - optional `app_cleanup`.

    Each returns 0 for pass and non-zero for fail. They write their output to
    stdout and stderr, which the harness captures to
    `build/corpus/<app>/<stage>.log`.
  - Helpers that `lib.sh` provides:
    - `guest <timeout_seconds> <guest-path> [args…]`: runs a program in the
      rootfs under Linuxemu, with `LINUXEMU_ROOT` set and
      `LINUXEMU_DIAGNOSTICS=build/corpus/<app>/diagnostics.txt`.
    - `guest_sh <timeout_seconds> '<busybox sh script>'`: runs a script with
      the rootfs `/bin/busybox sh -c`.
    - `guest_bg <pidfile> <guest-path> [args…]`: starts a guest in the
      background and records its host pid for cleanup.
    - `corpus_kill_bg`: kills every recorded background pid with TERM, then
      KILL after 3 s.
    - `rootfs_path <guest-path>`: prints the host path.
- Produces: `results.tsv` and `summary.txt` in the format above.

- [ ] **Step 1: Write the failing harness tests** in
  `tests/corpus-harness-test.sh`, each using a fake app:
  - `all-pass`: every stage returns 0. Expect five `pass` lines in stage
    order, `compatible=yes`, and the totals line `corpus apps=1 compatible=1`.
  - `fail-skips`: `stage_basic` returns 1. Expect `install`/`startup` pass,
    `basic` fail, `concurrent`/`shutdown` skip with note
    `after basic failed`, and `compatible=no`.
  - `fail-note`: a failing stage prints `boom\nsecond`. The note is `boom`.
  - `fail-cleanup`: `stage_basic` fails. `app_cleanup` still ran (it touches
    a marker file), and the fake world file matches its pre-run copy.
  - `hang-cleanup`: `stage_startup` sleeps past `CORPUS_STAGE_TIMEOUT=2`.
    Expect `startup` fail with note `timeout after 2s`, then cleanup ran.
  - `dirty-rootfs`: the app's virtual package (`corpus_virtual`) is already
    installed, from an earlier interrupted run. Expect `install fail` with
    note `rootfs not clean`, and every other stage skip.
    - The check is on the virtual package only. busybox and apk-tools are
      always installed in the base rootfs, so checking `corpus_packages`
      would wrongly flag them.
  - `undefined-stage`: the app doesn't define `stage_concurrent`. Expect
    `concurrent fail` with note `stage not defined`, then `shutdown skip`.
  - `continues-after-crash`: app A's stage `exit`s its shell. App B still
    runs and is scored.
  - `install-alone`: install passes and everything else fails. That gives
    `compatible=no`.
- [ ] **Step 2: Run the tests and confirm they fail.**
  Run `sh tests/corpus-harness-test.sh`. Expected: it fails, because
  `scripts/corpus/lib.sh` is missing.
- [ ] **Step 3: Implement `lib.sh` and `run-corpus.sh`.**
  - Each stage runs in a subshell, so an `exit` can't stop the runner.
  - The stage timeout comes from `CORPUS_STAGE_TIMEOUT` (default 300 s). The
    harness waits for the stage's subshell itself, rather than relying on
    `timeout(1)`, so the fake mode works on the host.
  - Cleanup order is: `corpus_kill_bg`, then `app_cleanup`, then
    `apk del <corpus_virtual>`, then the world check.
- [ ] **Step 4: Run the tests and confirm they pass.**
  Run `sh tests/corpus-harness-test.sh`. Expected:
  `corpus_harness_test_failures=0`.
- [ ] **Step 5: Commit.**

### Task 2: Pinned package closures

**Files:**
- Create `scripts/corpus/fetch-apks.sh <app> …`. It runs on the **device**.
  - It sources the app definition for `corpus_packages`.
  - Under Linuxemu it runs `apk fetch --recursive --output
    tmp/linuxemu-corpus-fetch <packages>` in the test rootfs, then moves the
    files to `artifacts/corpus/<app>/`.
  - It writes `docs/corpus/<app>-apks.sha256` with paths relative to the
    project dir, in the same format as `docs/python-runtime-apks.sha256`.
- Modify `scripts/corpus/lib.sh`. The default `stage_install`:
  - runs `sha256sum -c docs/corpus/<app>-apks.sha256` (fail note:
    `pinned archive check failed`);
  - copies the closure to the rootfs `tmp/linuxemu-corpus-apks`;
  - runs `apk add --no-network --no-chown --allow-untrusted --virtual
    <corpus_virtual> tmp/linuxemu-corpus-apks/*.apk`, the same flags as
    `run-python-smoke.sh`; and
  - checks `apk info -e` for every name in `corpus_packages`.
- Create the eleven app definition stubs in `scripts/corpus/apps/`, setting
  only `corpus_packages`. Their stage functions come in Tasks 3–4.
  - `busybox.sh`: `busybox`.
  - `apk.sh`: `apk-tools`.
  - `python.sh`: `python3`.
  - `git.sh`: `git`.
  - `sqlite.sh`: `sqlite`.
  - `vim.sh`: `vim`.
  - `gcc-make.sh`: `gcc make musl-dev`.
  - `nginx.sh`: `nginx`.
  - `node.sh`: `nodejs`.
  - `redis.sh`: `redis`.
  - `inotify-tools.sh`: `inotify-tools`.

**Interfaces:**
- Consumes: `lib.sh` (Task 1).
- Produces: `artifacts/corpus/<app>/*.apk` (ignored) and
  `docs/corpus/<app>-apks.sha256` (committed).

- [ ] **Step 1: Write the failing checks** in
  `tests/corpus-harness-test.sh`:
  - `tampered-archive` (fake mode, using a temporary manifest and a temporary
    archive whose bytes are changed): `install fail` with note
    `pinned archive check failed`, and the fake apk is never called.
- [ ] **Step 2: Run the test and confirm it fails.** Expected: it fails,
  because the default install has no manifest check yet.
- [ ] **Step 3:** Implement the fetch script and the default install. Run the
  fetch script on the Passport for all eleven apps. Run `sha256sum -c` on
  every manifest. Expected: all `OK`.
- [ ] **Step 4: Run the harness tests.** Expected:
  `corpus_harness_test_failures=0`.
- [ ] **Step 5:** Add `artifacts/corpus/` to `.gitignore` if `artifacts/`
  isn't already ignored. Commit the script, the manifests and the stubs.

### Task 3: Stage definitions for BusyBox, apk, Python, Git, SQLite and Vim

**Files:**
- Modify `scripts/corpus/apps/{busybox,apk,python,git,sqlite,vim}.sh`.

Stage definitions. Each one is a pass/fail check, and its expected values go
into the stage function:

| App | startup | basic | concurrent | shutdown |
|---|---|---|---|---|
| busybox | `busybox --help` prints `BusyBox v1.` | `seq 1 1000 \| sort -rn \| head -1` gives `1000`, and a tar+gzip round trip of a 3-file dir gives identical `md5sum` output | 4 background pipelines (`seq 1 20000 \| sort -n \| tail -1`) all give `20000`; `wait` exits 0 | `sleep 30 &`, TERM it; `wait` reports status 143; `kill -0` then fails |
| apk | `apk --version` prints `apk-tools 3.` | `apk info -e busybox` and `apk verify` on one pinned closure file exit 0 | 4 parallel `apk info -e busybox` all exit 0 | after the runs, no `lib/apk/db/lock` holder: `apk info` exits 0 within 10 s |
| python | `python3 -c 'import sys; print(sys.version_info[:2])'` prints `(3, 14)` | the existing `guest-tests/python-runtime-smoke.py`, passing as in `run-python-smoke.sh` | `ThreadingHTTPServer` on 127.0.0.1:18080 serves 10 parallel busybox `wget` clients, all HTTP 200 | SIGINT the server: exit 130, then 127.0.0.1:18080 refuses connections |
| git | `git --version` prints `git version 2.` | init, add, commit and `git log --oneline \| wc -l` = 1 in `tmp/corpus-git` | 4 parallel `git log` on the repo plus one `git gc --quiet`; all exit 0 | no `.git/index.lock` and no `gc.pid` remain |
| sqlite | `sqlite3 --version` prints `3.53` | create a table, insert 100 rows, `select count(*)` = 100 | WAL mode with `.timeout 5000`: 4 writers each insert 250 rows; the final count is 1100 | after `.quit` there is no `-wal` file and `pragma integrity_check` gives `ok` |
| vim | `vim --version` prints `VIM - Vi IMproved 9.2` | `vim -es -c '%s/a/b/g' -c wq` on a file; the content changes as expected | 4 parallel `vim -es` edits of 4 files, all correct | no `.swp` files are left in the edit dir |

Install is the default pinned install for all of them. For busybox and apk,
whose packages are already in the base rootfs, it reinstalls the pinned
closure under the virtual package and must exit 0.

- [ ] **Step 1:** Run `sh scripts/run-corpus.sh busybox` with the stubs from
  Task 2 on the Passport. Expected: every stage after install `fail`s with a
  "stage not defined" note. That proves the harness reports missing stages.
- [ ] **Step 2:** Implement the six definitions.
- [ ] **Step 3:** Run
  `sh scripts/run-corpus.sh busybox apk python git sqlite vim` on the
  Passport. Expected:
  - every stage has a result;
  - `etc/apk/world` is restored after each app; and
  - busybox, apk, python and git score `compatible=yes`, because the release
    regression already exercises them.

  SQLite and Vim results are recorded as measured.
- [ ] **Step 4: Commit.**

### Task 4: Stage definitions for GCC/make, nginx, Node.js, Redis and inotify-tools

**Files:**
- Modify `scripts/corpus/apps/{gcc-make,nginx,node,redis,inotify-tools}.sh`.

| App | startup | basic | concurrent | shutdown |
|---|---|---|---|---|
| gcc-make | `gcc --version` prints `15.2` | compile and run hello.c; output is `hello` | a Makefile with 4 independent objects, `make -j4`; the binary runs | no `cc*` temporaries left in `/tmp` |
| nginx | `nginx -t -c <corpus conf>` exits 0 | server on 127.0.0.1:18081; busybox `wget` of a 64 KiB file has a matching `md5sum` | 10 parallel `wget`s, all with matching md5 | `nginx -s quit`: the pid file is gone within 10 s and the port refuses connections |
| node | `node -e 'console.log(process.versions.node)'` prints `24.` | a script writes and reads back a file | an http server on 127.0.0.1:18082, 10 parallel `wget`s, all 200 | SIGTERM handler exits 0; the port refuses connections |
| redis | `redis-server --version` prints `v=8.8` | server on 127.0.0.1:18083; `redis-cli set k v` / `get k` gives `v` | `redis-benchmark -p 18083 -c 10 -n 1000 -t set,get -q` exits 0 | `redis-cli -p 18083 shutdown nosave`: the process exits within 10 s and the port refuses connections |
| inotify-tools | `inotifywait --help` output contains `inotifywait` (its exit status isn't checked) | `inotifywait -e create` on a dir sees a file created by busybox `touch` | 4 files created by 4 parallel `touch`es: `inotifywait -m` sees 4 create events | killing the watcher leaves no process, `wait` returns |

What "refuses connections" means depends on the app, because Alpine's BusyBox
may lack `nc -z`:
- for HTTP servers (python, nginx, node), busybox
  `wget -q -T 3 -O /dev/null http://127.0.0.1:<port>/` exits non-zero; and
- for Redis, `redis-cli -p 18083 ping` exits non-zero.

- [ ] **Step 1:** Implement the five definitions.
- [ ] **Step 2:** Run them on the Passport. Expected:
  - every stage has a result and the world is restored; and
  - the outcomes are whatever they are. inotify-tools is expected to fail at
    `basic` (no inotify yet, roadmap Phase 3), and nginx, Node and Redis may
    fail where they need epoll.

  Record each failing stage's note.
- [ ] **Step 3: Commit.**

### Task 5: First corpus baseline

**Files:**
- Create `docs/corpus-results.md`: the date, the Linuxemu commit, the device,
  the full `summary.txt`, and for each app its distinct unsupported-syscall
  names from `diagnostics.txt`, sorted by count.
- Modify `docs/FINDINGS.md`: add a dated entry summarising the baseline.
- Modify `README.md`: a short "Application corpus" section with how to run it,
  the result files, and that it is separate from the release regression.

- [ ] **Step 1:** Run `sh scripts/run-corpus.sh` on the Passport. Expected:
  - eleven apps scored;
  - a `corpus apps=11 compatible=<n>` totals line; and
  - every app's world restored.
- [ ] **Step 2:** Run the full regression. Expected:
  `linuxemu_full_regression_failures=0`, which shows the corpus left no
  residue that breaks it.
- [ ] **Step 3:** Write the three documents from the run's files.
- [ ] **Step 4: Commit.**
