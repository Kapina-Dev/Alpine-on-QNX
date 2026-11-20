# Phase 3, part 3: inotify — design

Date: 2026-11-20. Roadmap: `docs/capability-roadmap.md`, "Phase 3: Linux
event interfaces" ("### inotify"). Parts 1 (eventfd, epoll) and 2
(timerfd, signalfd) are merged and pushed (`36540e5`). **Approach chosen by
the user: wrap QNX's native inotify descriptor (option 1).**

## Intent

Part 3 adds Linux `inotify` to the Phase 1 descriptor layer, so it works
with `read`, `poll`/`select`, epoll, `ioctl(FIONREAD)`, `dup`, `fork`,
`exec` and `SCM_RIGHTS` the way the part 1 and part 2 objects do. It is the
last part of Phase 3; the user releases once after it.

The concrete consumer is the corpus app **inotify-tools**, which today
fails at its basic stage because `inotify_init` returns `ENOSYS`. Others
are libuv/Node.js `fs.watch`, and later Android's FileObserver and
PackageManager (`IN_CREATE`, `IN_DELETE`, `IN_MOVED_*`, `IN_CLOSE_WRITE` on
directories).

## What the device probe found (2026-11-20)

`native-probes/phase3c-probes.c`, run on the Passport as the ordinary user
(devuser, uid 101431000). Details go into `docs/FINDINGS.md` in the first
plan task.

- **QNX already has inotify.** libc exports `inotify_init`,
  `inotify_add_watch` and `inotify_rm_watch` (`<sys/inotify.h>`, Android's
  header with the Linux values). They work through `/dev/fsnotify`
  (`root:Disk_Drivers`, `rw-rw-r--`); the ordinary user can open it
  read-only (`O_RDWR` gives `EACCES`), and `inotify_init` succeeds.
  `/dev/fsevents` is likewise read-only for us. There is no `inotify_init1`.
- **The records are Linux's.** `struct inotify_event` has the same 16-byte
  header and the same mask bits; names are NUL-padded to a multiple of 16
  (`len` 16 for short names), as on Linux. Watch descriptors are numbered
  per instance from 1; re-adding a path returns the same wd and replaces
  the mask.
- **On qnx6 (the rootfs's filesystem) the events match Linux:** `CREATE`,
  `OPEN`, `ACCESS`, `MODIFY`, `ATTRIB`, `CLOSE_WRITE`, `CLOSE_NOWRITE`,
  `DELETE`, `ISDIR` on directory entries, `MOVED_FROM`/`MOVED_TO` with a
  shared nonzero cookie, `DELETE_SELF` then `IGNORED` when the watched
  directory or file is removed, `IGNORED` after `inotify_rm_watch`. Changes
  in a subdirectory are not reported to the parent's watch (Linux is not
  recursive either). Changes made by other processes are reported.
- **Flags that work natively:** `IN_ONESHOT` (one event, then `IGNORED`),
  `IN_MASK_ADD`, `IN_ONLYDIR` (`ENOTDIR` on a file), `IN_DONT_FOLLOW`
  (watches the symlink itself), `IN_Q_OVERFLOW` when the queue fills (about
  2,000 events queued).
- **Descriptor behaviour:** a blocking `read` with nothing queued blocks
  and is woken by an event or ended with `EINTR` by a signal; a read
  buffer too small for the next event gives `EINVAL` (as Linux); `poll`
  reports `POLLIN`; `ioctl(FIONREAD)` returns the queued byte count; a
  forked child's copy shares the queue (one open-file description).
  Errors: missing path `ENOENT`, unknown wd `EINVAL`. At least 300
  instances per process and 2,000 watches per instance.
- **Where QNX differs from Linux:**
  1. A **nonblocking** `read` with nothing queued returns **0**; Linux
     returns `EAGAIN`.
  2. A **zero event mask** is accepted; Linux returns `EINVAL`.
  3. Watches on **`/dev/shmem`, `/pps` and `/proc` fail with `ENOTTY`**.
     qnx6, the root filesystem, rcfs (`/base/bin`) and the exFAT sdcard
     accept watches. Linuxemu's guest `/tmp` is the rootfs's `tmp` on
     qnx6, so it works; a guest path that resolves into `/dev/shmem` does
     not.
  4. **Delivery is batched**: an event reaches `poll` about 250 ms after
     the change (median 254 ms, max 286 ms over 40 trials; about 25 ms when
     a batch boundary happens to be near). Linux promises no latency, but
     its events arrive at once.
  5. Background system processes on the Passport (indexers) open new files
     and directories shortly after they change, so a watch with `IN_OPEN`
     or `IN_CLOSE_NOWRITE` sees extra events. They are real events from
     other processes, as Linux would report them.
- **A stat-scanning fallback is not viable:** `readdir` + `lstat` of a
  1,000-entry directory took 4.2 s, and directory `st_mtime` has whole-second
  resolution.
- `ionotify()` on a directory fd returns `EINVAL`: no other native
  change-notification path.

Follow-up facts (`native-probes/phase3c-facts.c`, same day, answering the
builder check):

- **Identity:** `fstat` on every inotify fd reports `/dev/fsnotify`'s own
  `st_dev`/`st_ino` (1/871157257 on this device; mode `020664`, a
  character device). Two instances are indistinguishable by `fstat`.
  `/dev/null` reports a different identity. So "this fd is an inotify fd"
  can be recognised by comparing with `stat("/dev/fsnotify")`.
- **`execve`:** the native fd survives and keeps delivering events in the
  new image.
- **`dup`, `F_DUPFD`, `dup2`** work natively on an inotify fd.
- **`SCM_RIGHTS` fails natively:** `sendmsg` succeeds, but the receiver's
  `recvmsg` fails with `EACCES`, and the message's data is lost along with
  the fd.
- **Overflow record:** `wd` -1, mask `IN_Q_OVERFLOW` (0x4000), cookie 0,
  len 0, which is Linux's record. QNX queues it **first**, ahead of the
  2,048 queued events; Linux appends it at the end of the queue.
  `FIONREAD` on a full queue reported 65,536.

## Goals

- `inotify_init`, `inotify_init1` (`IN_NONBLOCK`, `IN_CLOEXEC`),
  `inotify_add_watch` and `inotify_rm_watch` with Linux semantics, as far
  as the native backend provides them, on the guest's paths.
- An inotify fd works with `read` (blocking and nonblocking), `readv`,
  `poll`/`select`/`ppoll`/`pselect6`, epoll (level- and edge-triggered),
  `ioctl(FIONREAD)`, `fcntl` `O_NONBLOCK`/`FD_CLOEXEC`, `dup`/`dup2`/`dup3`,
  `fork`, `execve` and `SCM_RIGHTS`, the same way part 1 and part 2 objects
  do.
- The watcher cases the roadmap names: create, update, rename (paired
  cookie), delete, delete-self, one-shot, overflow, duplication.
- Corpus: **inotify-tools becomes compatible** (all four stages: startup,
  basic, concurrent, shutdown).
- Every existing suite stays green; the full device regression passes with 0
  failures; the corpus has no regressions (baseline 7 of 11 compatible, plus
  Redis since part 1).

## Non-goals

- A relay thread or emulated event queue (option 2, not chosen).
- A stat-scanning fallback for filesystems QNX can't watch.
- Lowering the ~250 ms batching delay (it is QNX's).
- `/proc/sys/fs/inotify/*` limit files (synthetic `/proc` is Phase 5).
- fanotify.
- The rest of the Phase 3 acceptance gate that inotify doesn't decide:
  nginx's `FIOASYNC`/`F_SETOWN` gap and Node.js's `madvise`/`mremap`
  crash. They stay recorded as open in the roadmap (see "Gate").
- Isolating the io-pkt crash (needs the user present; separate follow-up).
  `scripts/run-iopkt-stress.sh` is not run.

## Anti-goals

- Success stubs: a watch Linuxemu can't honour returns a Linux error, and
  the limitation is documented. Never a watch that silently never fires.
- No polling loops, no extra threads, no hidden descriptors for inotify.
- Linuxemu must never message io-pkt from a signal handler (unchanged rule;
  inotify doesn't touch io-pkt at all).

## Constraints (from the code and earlier phases)

- The fd layer (`src/guest_fd.h`): guest fd numbers are native QNX numbers;
  a number may carry a reference-counted typed object with ops (`read`,
  `poll_fd`, `get_status`, `set_status`, `serialize`, `ready_now`,
  `companions`, `release`). `GUEST_FD_INOTIFY` is already in the enum.
  Objects are identified by `fstat64` (`st_dev`, `st_ino`) of the backing
  fd, to drop records whose number was reused behind the table's back.
- Syscalls run inside the SIGILL handler: no `malloc`/stdio in dispatch
  paths. Builds are warning-free (`-Wall -Wextra`).
- Guest paths go through `guest_path_resolve` / `guest_path_resolve_nofollow`
  (rootfs prefix, guest symlinks).
- Tests: C guest fixtures in `guest-tests/`, run by
  `scripts/run-event-smoke.sh` from inside the rootfs; a distinct exit code
  per check; every wait bounded (BerryCore `timeout` never fires).
- The corpus test watches `/tmp/corpus-watch` inside the guest and waits
  2 s after starting the watcher and 2 s after the changes.
- Device workflow: `ssh -o HostName=<address> bb10`; the dev checkout is
  `/accounts/1000/shared/misc/linuxemu-dev`; `run-full-regression.sh`
  needs a terminal (`ssh -tt`, no pipes).

## Approaches considered

1. **Wrap the native inotify fd (chosen).** The guest's inotify fd *is* the
   native QNX inotify fd, carrying a `GUEST_FD_INOTIFY` object whose ops fix
   the few differences. Readiness, blocking, fork sharing and exec survival
   come from the native descriptor. Small; no threads, no hidden fds.
2. **Relay thread.** One emulator thread per process reads the native queue
   and refills an emulated queue with part 1's eventfd readiness channel.
   It would let Linuxemu rewrite events and fake watches on unsupported
   filesystems, but costs a thread and hidden fds per process and makes
   fork, exec and `SCM_RIGHTS` hard (the queue would no longer be one
   shared open-file description). Not chosen: nothing in the probe needs
   rewriting.
3. **Stat-scanning backend.** Ruled out by the probe (4.2 s per 1,000-entry
   scan, 1 s mtime granularity).

The user's original note to reuse part 1's eventfd readiness helpers
applied to option 2; option 1 doesn't need them, because the native fd is
pollable.

## Design

### Object

`src/linux_inotify.c` (new) registers a `GUEST_FD_INOTIFY` type whose
backing fd is the guest's fd itself. The object holds **no state of its
own**: the queue, watches and `O_NONBLOCK` all live on the native open-file
description. That makes duplicates, fork copies and adopted copies correct
by construction, and makes it harmless if two different inotify
descriptors share an object identity (every inotify fd may report the same
`/dev/fsnotify` `st_dev`/`st_ino`, which the follow-up probe confirmed).

The fd layer's `adopt_received` matches any non-path object by identity.
Since every inotify fd has the same identity, a stale or received inotify
fd may adopt another inotify fd's object. That is correct because the
object holds no state, but the builder must make sure no *other* type can
share `/dev/fsnotify`'s identity (none can: no other object is backed by
`/dev/fsnotify`).

### Calls

- `inotify_init` / `inotify_init1(flags)`: flags other than `IN_NONBLOCK`
  (Linux `O_NONBLOCK`, 0x800) and `IN_CLOEXEC` (Linux `O_CLOEXEC`) →
  `EINVAL`. Native `inotify_init()`, then `guest_fd_admit` (`EMFILE` if it
  landed in the hidden range), `fcntl` for the flags, attach the object.
  Native errors are mapped (`EACCES` on `/dev/fsnotify` stays `EACCES`;
  Linux's `EMFILE` for too many instances).
- `inotify_add_watch(fd, path, mask)`:
  - `fd` must carry the inotify object: `EBADF` for a bad fd, `EINVAL` for
    another kind of fd.
  - `mask` with no event bits (`mask & IN_ALL_EVENTS == 0`) → `EINVAL`
    (QNX accepts it). `IN_MASK_ADD | IN_MASK_CREATE` → `EINVAL`.
    `IN_MASK_CREATE` alone (Linux 4.18; QNX doesn't know it) → `EINVAL`,
    documented. Unknown high bits are dropped, as Linux ignores them.
  - The path is resolved as a guest path: `guest_path_resolve`, or
    `guest_path_resolve_nofollow` with `IN_DONT_FOLLOW`. Resolution errors
    are returned as they are (`ENOENT`, `ENOTDIR`, `ELOOP`,
    `ENAMETOOLONG`, `EFAULT`).
  - Native `ENOTTY` (filesystem without notification support) →
    **`EOPNOTSUPP`** with a diagnostic note; documented.
- `inotify_rm_watch(fd, wd)`: same fd checks; native result (unknown wd is
  `EINVAL`, as on Linux).
- Event names are relative to the watched directory, so nothing is
  translated on the way out.

### read

The object's `read` op calls the native `read` into the guest buffer. A
return of 0 means an empty queue on a nonblocking description (Linux never
returns 0 from an inotify read): return `EAGAIN`. Too-small buffers stay
`EINVAL`; `EINTR` stays `EINTR`. `readv` goes through the existing typed
readv path. After the read, the usual `linux_epoll_note_io(fd)` for
`EPOLLET` re-arm.

Native blocking reads can't be interrupted by Linuxemu's own wake-ups any
differently from other native reads: the existing signal model applies
(the probe showed `EINTR` on a signal).

### Readiness, ioctl, fcntl

- `poll_fd` is the fd itself; no `ready_now` op. poll, select and epoll
  watch the native fd.
- `ioctl(FIONREAD)` already maps to the native `FIONREAD`
  (`src/linux_termios.c`), which works on the native fd. The plan checks
  that typed fds reach it.
- `get_status`/`set_status` are the native `fcntl` calls (or absent, so
  the native path runs), so `O_NONBLOCK` set through any duplicate is seen
  by all of them.

### dup, fork, exec, SCM_RIGHTS

- `dup` family: the layer's `guest_fd_duplicate` shares the object.
- `fork`: the native fd is inherited and shares the queue (probe); the
  object table is inherited as for other types.
- `execve`: the native fd survives (no `FD_CLOEXEC`) and still delivers
  (probe). A `serialize` op writes an exec record with no hidden fd
  (`restore_one` already accepts a hidden value of -1), and the restore
  hook re-attaches the object in the new image.
- **Recognition by identity:** `src/linux_inotify.c` records
  `stat("/dev/fsnotify")`'s identity at registration. Wherever the layer
  meets a number with no record that has this identity (exec restore
  fallback, `guest_fd_received`), it attaches an inotify object. The
  builder chooses the hook points.
- **`SCM_RIGHTS`: QNX can't pass an inotify fd** (receiver `EACCES`,
  message lost). Linuxemu's `sendmsg`/`sendmmsg` checks the guest's fds
  first, and **if any is an inotify fd, fails with `EOPNOTSUPP` and sends
  nothing**, so the receiver never loses a message. Documented. Linux would
  pass it; no corpus app or Android path passes inotify fds.

### Diagnostics and names

The four syscall numbers are already in `src/linux_syscall_names.h`
(316, 317, 318, 360). Unsupported-filesystem watches log a diagnostics
note under `LINUXEMU_DIAGNOSTICS`.

## Tests

C guest fixtures in `guest-tests/`, added to `scripts/run-event-smoke.sh`,
all waits bounded and every check with a distinct exit code. Events can be
~250 ms late, so fixtures wait with `poll` for up to 2 s per expected
event, and treat extra `OPEN`/`CLOSE_NOWRITE`/`ACCESS` events from other
processes as noise (filter by mask, or watch only the bits under test).

- **`inotify-basic`**: init/init1 flags (`IN_NONBLOCK` gives `EAGAIN` on
  an empty queue, `IN_CLOEXEC` sets `FD_CLOEXEC`, bad flags `EINVAL`);
  create, modify, close-write, attrib, delete, rename with paired cookie,
  delete-self + `IGNORED`; `rm_watch` gives `IGNORED`; zero mask and bad
  wd give `EINVAL`; missing path `ENOENT`; `IN_ONLYDIR` on a file
  `ENOTDIR`; a non-inotify fd gives `EINVAL`; a too-small buffer gives
  `EINVAL`; `FIONREAD`. The unsupported-filesystem `EOPNOTSUPP` mapping
  can only be reached from a guest path that resolves to a native device
  path (`resolve_guest_device` maps only `/dev/null` and `/dev/pts/*`).
  The builder tries `/dev/pts/N`, which may report `ENOTTY` on QNX, and
  otherwise skips that check with a note.
- **`inotify-modes`**: one-shot; `IN_MASK_ADD`; re-adding returns the same
  wd; overflow after filling the queue (an `IN_Q_OVERFLOW` record with
  wd -1 and len 0 appears *somewhere* in what is read, because QNX puts it
  first); poll,
  select, and epoll level- and edge-triggered on an inotify fd; a blocking
  read woken by an event from another thread, and one ended by a signal
  with `EINTR`.
- **`inotify-share`**: `dup` shares the queue and `O_NONBLOCK`; a forked
  child reads events the parent's watch produced; the fd survives
  `execve` and still works in the new image; `sendmsg` with an inotify fd
  in `SCM_RIGHTS` fails with `EOPNOTSUPP`, and a following plain message
  on the same socket still arrives.
- **Corpus**: `scripts/corpus/apps/inotify-tools.sh` as it is; the header
  comment saying "expected to fail" is updated.

## Device probes (first plan task)

The probes are done (above). The first plan task adds both probe programs
to `scripts/build-native-probes.sh`, records the findings in
`docs/FINDINGS.md`, and commits them. Nothing in the build waits on a new
probe. `IN_EXCL_UNLINK` was not probed: it is passed through and listed as
untested.

## Gate for part 3

- New fixtures `inotify-basic`, `inotify-modes`, `inotify-share` pass on
  the Passport.
- The corpus run shows inotify-tools compatible (all four stages) and no
  regression elsewhere.
- Full device regression: `linuxemu_full_regression_failures=0`.
- `docs/compatibility.md` ("Limits of the Phase 3 part 3 calls"),
  `docs/FINDINGS.md`, the roadmap's Phase 3 status and dependency chart,
  and `docs/development.md` updated, including every limitation above.
- Phase 3 as a whole: the roadmap gate's libuv item (not run in part 3,
  by the user's choice) and its nginx and Node.js items (blocked by
  non-inotify gaps: nginx `FIOASYNC`, Node.js `madvise`/`mremap`) stay
  open. The status section says so plainly rather than marking Phase 3
  complete.

## Decided by the user

- Option 1: wrap the native inotify fd.
- The corpus target is inotify-tools, **and only inotify-tools**: libuv's
  `fs_event` tests are not run in part 3. The roadmap's libuv gate item
  stays open alongside nginx and Node.js.
- Same flow as part 2: spec, plan with Global Constraints, subagent-driven
  development in a worktree branch.
- One release after part 3.
- Don't run `run-iopkt-stress.sh`.

## My calls (correct me)

- A watch on a filesystem QNX can't watch (`/dev/shmem`, `/pps`, `/proc`)
  returns **`EOPNOTSUPP`** and is documented, rather than a watch that
  never fires.
- `IN_MASK_CREATE` returns `EINVAL` (QNX can't tell us whether a watch
  already exists without tracking every watch).
- The ~250 ms delivery delay is documented, not worked around.
- Extra events from Passport background processes are real events and are
  passed through.
- Received and exec-carried inotify fds are recognised by
  `/dev/fsnotify`'s identity (the probe confirmed this works).
- A guest `sendmsg` that passes an inotify fd fails with `EOPNOTSUPP`
  and sends nothing, because QNX can't pass the fd and would drop the
  whole message at the receiver.
- The overflow record is passed through where QNX puts it (first, not
  last); documented.
- Phase 3 is not marked complete while nginx and Node.js are blocked by
  non-inotify gaps; those stay listed in the roadmap.

## Left to the builder

- The exact exec-record form (or identity-based retyping) and the receive
  hook.
- Whether `get_status`/`set_status` ops are needed or the native path
  suffices.
- Fixture layout within the three files, and task order.
