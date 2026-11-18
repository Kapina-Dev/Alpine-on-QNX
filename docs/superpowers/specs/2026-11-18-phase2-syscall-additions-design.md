# Phase 2: contained modern syscall additions — design

Date: 2026-11-18. Roadmap: `docs/capability-roadmap.md`, "Phase 2: contained
modern syscall additions". Approach chosen by the user: **A, one Phase 2
branch delivered in groups, Node first.**

## Intent

Phase 2 adds standalone Linux system calls that real apps make and Linuxemu
doesn't yet handle. Each call can be implemented and tested on its own. The
user follows the roadmap phases in order. They widened this phase with two
decisions:

1. **Include the Node.js start-up fix.** Node aborts at start-up because
   `sigaction(n, NULL, &old)` returns EINVAL for Linux signals that have no
   QNX equivalent (`docs/corpus-results.md`).
2. **Include the calls the application corpus shows apps hitting**, not only
   the roadmap's list. This follows the roadmap principle "add functionality
   in response to reproducible workloads".

Success means the corpus apps stop hitting ENOSYS for these calls, Node gets
past its signal abort with its next blocker recorded, and every existing
regression stays green. The user said Node isn't the priority: as far as
Phase 2 takes it is fine.

## Goals

- Every call in the scope table below has a Linux-correct implementation or
  a deliberate Linux error, and a direct ARM guest test.
- Node.js gets past the `sigaction` abort, and the call it stops on next is
  recorded in `docs/corpus-results.md`. Node is expected to stop on
  `epoll_create1` inside the start-up stage (libuv creates its loop before
  `node -e` prints anything); that is the hand-off to Phase 3.
- The corpus score doesn't drop, and the per-app unsupported-call lists in
  `docs/corpus-results.md` shrink by the calls implemented here.
- The full 14-suite device regression passes with 0 failures.

## Non-goals

- `epoll`, `inotify`, `timerfd`, `signalfd` and full `eventfd`. These are
  Phase 3, even though nginx, Redis and inotify-tools hit them in the corpus.
  If Node's next blocker after start-up is `epoll`, that's the expected
  hand-off to Phase 3, not a reason to pull epoll forward.
- Real Linux capabilities, scheduling policies or CPU pinning semantics
  beyond what QNX can actually do.
- Shared-address-space `vfork`, process-shared futexes, synthetic `/proc`
  (later phases).

## Anti-goals

These would make the phase a failure even if the tests pass:

- **Fake success.** A call must never report success for something that did
  not happen (for example `sched_setaffinity` returning 0 without pinning,
  or `capset` raising a capability). The roadmap requires "real semantics
  where possible and proper errors otherwise". Report the unsupported case
  through `linux_unsupported()` so it shows in `LINUXEMU_DIAGNOSTICS`.
- **Regressions in existing workloads.** Python, Git, apk, network, signal
  and thread suites must stay green after every group, not only at the end.
- **Raw Linux constants reaching QNX.** Linux and QNX flag values differ
  (for example Linux `MAP_NORESERVE` 0x4000 is QNX `MAP_NOINIT`). Every flag
  is translated explicitly; unknown bits are rejected.

## Constraints

- Host: BlackBerry Passport, QNX 8.0.0 / BB10 10.3.1 SDK, ordinary user.
  Code is built on the device with `scripts/build-native-probes.sh`. The
  device has no git; source is copied to
  `/accounts/1000/shared/misc/linuxemu-dev`.
- Guest: Alpine 3.24 armhf (musl), 32-bit ARM EABI syscall numbers.
- The Phase 1 descriptor layer (`src/guest_fd.c`) is authoritative for
  descriptor numbers: every descriptor-creating path calls
  `guest_fd_admit`/`guest_fd_clear`, descriptors at or above
  `guest_fd_hidden_base()` are not the guest's (EBADF/EMFILE), and dups go
  through `guest_fd_duplicate`. New calls must keep these rules.
- Signal-safety: the dispatcher runs inside the QNX SIGILL handler. New code
  must not call non-async-signal-safe functions where the existing code
  avoids them (no `malloc` in hot paths, no stdio).
- Device test gotchas (from Phase 0/1): `run-full-regression.sh` needs a
  terminal (`ssh -tt`, no pipes or redirects); piping data into `ssh -tt`
  hangs, so upload without `-tt`; BerryCore `timeout` never fires on QNX.

## What QNX provides (checked in the SDK headers, 2026-11-18)

| Host facility | Present |
| --- | --- |
| `getrusage`, `setpriority`/`getpriority` (`sys/resource.h`) | yes |
| `posix_fallocate` (`fcntl.h`) | yes |
| `sendmmsg`/`recvmmsg` (`sys/socket.h`) | yes |
| `ThreadCtl(_NTO_TCTL_RUNMASK)` CPU affinity (`sys/neutrino.h`) | yes |
| `sysconf(_SC_NPROCESSORS_ONLN)` | yes |
| `fstatvfs`/`statvfs`, `fchmod` | yes |
| `msync(MS_INVALIDATE_ICACHE)` (already used by Linuxemu) | yes |
| `preadv`/`pwritev` | **no** |
| `sendfile` | **no** |

## Approaches

- **A. One branch, grouped by area, Node first (chosen).** One worktree
  branch, five groups, each a tested, committed step with device checks.
  Full regression and corpus rerun at the end. Matches Phase 1's shape;
  Node goes first so its next blocker is known early.
- **B. Node as its own quick release, then Phase 2.** Sees Node's next
  failure sooner, but costs a second release cycle and a second corpus run
  for two small fixes.
- **C. Group by app** (everything SQLite needs, then GCC, ...). Tracks the
  corpus closely, but shared calls such as `statx` split across groups and
  tests overlap.

The user chose A.

## Scope and behaviour

ARM EABI numbers in brackets. "Unsupported" means: return the stated Linux
error through `linux_unsupported()` with a short `detail` tag.

### Group 1: Node.js

- **`rt_sigaction` on unmapped signals [174].** Linux signals 16
  (SIGSTKFLT) and 48–64 have no QNX equivalent (QNX has 16 real-time
  signals, mapped to Linux 32–47). Today `guest_signal_action` returns
  EINVAL for them before doing anything. New behaviour, matching Linux:
  - Querying (`act == NULL`) works for every signal 1–64 and returns the
    stored action (initially SIG_DFL).
  - Setting works for every signal except 9 and 19 (EINVAL as now). For an
    unmapped signal the action is stored only; no host `sigaction` happens.
    Every *send* of an unmapped signal (`kill`, `tkill`, `tgkill`,
    `rt_sigqueueinfo`, `rt_tgsigqueueinfo`, to self or others) returns
    EINVAL with detail `signal-unmapped`. Today's self-delivery runs through
    host signal numbers and the pending queue covers only 32–47, so real
    in-process delivery (handler, blocking, SIG_IGN, default termination)
    would be a new signal subsystem; it is out of scope. Node needs only
    query and store. (Designer's call, from the review.)
  - Signal numbers 0 and >64 stay EINVAL.
- **ARM `cacheflush` [0x0f0002] (ARM private syscall).** V8's JIT calls it
  after writing machine code. BerryCore's native QNX Node port forces
  `--jitless` because its JIT hit SIGBUS, which points at the same cache
  coherence problem. Implement `cacheflush(start, end, flags)`: validate
  `start < end`, `flags == 0` (else EINVAL), round to cache lines/pages,
  and run the existing `msync(MS_INVALIDATE_ICACHE)` path on the range.
  If the range isn't mapped guest memory, return EINVAL (Linux returns
  EINVAL/EFAULT for bad ranges). `cacheflush` does not patch anything.
- **Known hazard, measured but not fixed here.** `linux_mprotect_runtime`
  runs `arm_patch_range` over the whole range every time an anonymous
  runtime mapping becomes executable. That blind scan rewrites any data
  word equal to `svc #0` (0xef000000) or a TPIDRURO read (for example in a
  JIT literal pool) into a trap, and repeated flips can fill the fixed
  patch table (`mprotect` then fails with E2BIG). Group 1 adds a guest
  test that writes such a literal pool, flips it RX, and checks the data,
  plus a loop of RW→RX flips, so the hazard is measured and recorded in
  `docs/FINDINGS.md`. The fix (patching only real instructions, which needs
  code/data knowledge Linuxemu doesn't have for JIT memory) belongs with
  Phase 4's "Thumb and stripped-binary support" patcher work. If Node
  reaches its JIT in this phase and fails because of it, Node can be
  measured with `--jitless` meanwhile. (Designer's call, from the
  review.)
- **Acceptance for the group:** a direct ARM test covering both calls, and
  the corpus Node app no longer aborting in `ResetSignalHandlers`. Record
  the call it stops on next (expected: `epoll_create1`) in
  `docs/corpus-results.md`. Node passing start-up is not required in this
  phase.

### Group 2: descriptors and file I/O

- **`pipe2` [359], `dup3` [358], `accept4` [366].** Already implemented
  with flag handling. Work here is the roadmap's test coverage: valid use,
  invalid flags (EINVAL), `dup3` with equal fds (EINVAL), replacing an open
  destination including a typed (eventfd) one, hidden-range destinations
  (EBADF), and concurrent `dup3` + `close` from two threads. Fix whatever
  the tests find. `pipe2` with Linux `O_DIRECT` (packet mode): EINVAL,
  unsupported detail `pipe2-packet`.
- **`preadv` [361], `pwritev` [362], `preadv2` [392], `pwritev2` [393].**
  QNX has no vector positional I/O. Emulate with a loop of `pread`/`pwrite`
  per iovec, advancing the offset, stopping at the first short transfer, and
  returning the total (Linux semantics for partial I/O; an error after some
  bytes were transferred returns the byte count). The file offset must not
  move. 64-bit offset arrives split across two registers (low, high) as in
  the Linux ARM ABI. For the `*2` variants: offset −1 means "use and update
  the current file position" (fall back to `readv`/`writev`); flags 0 only;
  `RWF_HIPRI`/`RWF_DSYNC`/`RWF_SYNC`/`RWF_NOWAIT`/`RWF_APPEND` → EOPNOTSUPP,
  unsupported detail `rwf-flags`, except `RWF_DSYNC`/`RWF_SYNC`, which may
  be honoured by a following `fsync` (builder's call, see below).
  `iovcnt` > 1024 or negative: EINVAL. Not atomic across iovecs, unlike
  Linux for regular files; record as a known limit.
- **`fallocate` [352].** Mode 0 → `posix_fallocate` (translate its returned
  error number; it doesn't set errno). `FALLOC_FL_KEEP_SIZE` and every other
  mode (punch hole, collapse, zero range) → EOPNOTSUPP, unsupported detail
  `fallocate-mode`. Negative offset/length → EINVAL.
- **`fcntl64` additions [221].** Add `F_GETOWN`/`F_SETOWN` (8/9, QNX
  equivalents if present, else EINVAL with detail), `F_GETPIPE_SZ`
  (1032, report the QNX pipe buffer size from `fpathconf(_PC_PIPE_BUF)` or
  a measured constant) and `F_SETPIPE_SZ` (1031, accept requests up to the
  reported size and return it; larger → EPERM as for an unprivileged Linux
  process), OFD locks `F_OFD_GETLK`/`F_OFD_SETLK`/`F_OFD_SETLKW` (36/37/38)
  mapped onto the existing record-lock path with a documented
  approximation (per-process, not per-open-file), and `F_ADD_SEALS`/
  `F_GET_SEALS` (1033/1034) → EINVAL (no memfd). Unknown commands stay
  EINVAL.
- **`sendfile64` [239].** Emulate with a bounded read/write loop through a
  fixed stack buffer (e.g. 16 KiB). With an `offset` pointer: `pread` from
  that offset and write back the advanced offset, leaving the input file
  position alone. Without: plain `read`. Return bytes transferred; stop at
  EOF or a short write; nonblocking output returning EAGAIN after some bytes
  returns the count.

### Group 3: file metadata

- **`statx` [397].** Every corpus app calls it (Python 1,726 times) and
  falls back to `fstatat64` on ENOSYS. Implement on top of the existing
  `fstatat64` path (same path containment and `AT_*` flag handling,
  including `AT_EMPTY_PATH` and `AT_SYMLINK_NOFOLLOW`). Fill the
  `STATX_BASIC_STATS` fields and set `stx_mask` to exactly the fields
  filled; `stx_btime` is not available (leave its mask bit clear). Accept
  and ignore `AT_STATX_SYNC_TYPE` values. Unknown flag bits → EINVAL. Mask
  bits the caller asks for that can't be filled are simply not set in
  `stx_mask` (Linux behaviour), not an error.
- **`statfs64` [266] and `fstatfs64`.** `fstatfs64` exists; add `statfs64`
  through the guest path translation and the same struct conversion. The
  `sz` argument is accepted as 84 or 88, as the Linux ARM kernel does
  (musl armhf passes 88, the OABI-compat size is 84); anything else is
  EINVAL. Align `fstatfs64` (which today accepts any size ≥ 88 and writes
  88) to the same rule through one shared conversion helper, writing only
  `sz` bytes.
- **`fchmod` [94].** Straight to QNX `fchmod`, hidden-range fds EBADF.
- **`faccessat2` [439].** As the existing `faccessat`, plus a `flags`
  argument: `AT_EACCESS` and `AT_SYMLINK_NOFOLLOW` supported where the
  existing path handling can honour them, `AT_EMPTY_PATH` as for `statx`;
  unknown bits EINVAL.
- **`chown32` [212] (and `fchown32`/`lchown32`/`fchownat` if absent).**
  QNX `chown`/`lchown`/`fchown` through guest path translation. An
  unprivileged caller changing to its own uid/gid succeeds; anything else
  returns QNX's EPERM translated. `-1` means "don't change" on both.
- **`mknod` [14] (and `mknodat`).** Support `S_IFIFO` (via QNX `mkfifo`)
  and `S_IFREG` (create an empty file). Device nodes (`S_IFCHR`,
  `S_IFBLK`) → EPERM as for an unprivileged Linux process. `S_IFSOCK`:
  EPERM with detail (sockets are created by `bind`).

### Group 4: sockets

- **`sendmmsg` [374], `recvmmsg` [365].** Implement as a loop over the
  existing single-message `sendmsg`/`recvmsg` translation (addresses, flags,
  `SCM_RIGHTS`, `MSG_CMSG_CLOEXEC`), so every per-message rule is shared.
  Fill `msg_len` per entry. Return the number of messages processed; an
  error on the first message returns the error, a later one returns the
  count so far. `vlen` above 1024 is clamped (Linux `UIO_MAXIOV`).
  `recvmmsg`'s `MSG_WAITFORONE` → nonblocking after the first message;
  its timeout argument is honoured only between messages, as on Linux.
  Also reachable through `socketcall` if it has slots for these.

### Group 5: process and resources

- **`getrlimit` [76, if used], `ugetrlimit` [191], `setrlimit` [75].**
  Route through the existing `linux_prlimit64` (NOFILE stays virtual).
  32-bit `struct rlimit`: values ≥ 2^32−1 report as `RLIM_INFINITY`
  (0xffffffff). Add `RLIMIT_STACK` behaviour from the Android list: report
  QNX's value; a guest raising it beyond the hard limit gets EPERM.
- **`getrusage` [77].** `RUSAGE_SELF`, `RUSAGE_CHILDREN` via QNX
  `getrusage`, converted field by field to the Linux 32-bit `struct
  rusage` (timevals with 32-bit seconds). `RUSAGE_THREAD` (1): fill
  user/system time from the thread CPU clock that `clock_gettime` already
  supports, other fields 0. Unknown `who` → EINVAL.
- **`sysinfo` [116].** Fill the Linux 32-bit `struct sysinfo`: `uptime`
  from the monotonic clock, `totalram`/`freeram` from QNX's memory
  information (the builder picks the QNX source, for example
  `/proc/self/as` or `sysconf`, and documents it), `procs` best-effort,
  `mem_unit` 1 (or larger if totals overflow 32 bits), loads 0 (QNX has no
  load average: document it).
- **`setpriority` [97], `getpriority` [96].** `PRIO_PROCESS` for self and
  own children through QNX; Linux's `getpriority` returns `20 - nice`, so
  convert. `PRIO_PGRP`/`PRIO_USER` → EINVAL with detail if QNX lacks them.
  Raising priority (lower nice) unprivileged → EACCES as on Linux.
- **`sched_getaffinity` [242], `sched_setaffinity` [241].** QNX runmask via
  `ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET)`. `getaffinity` returns the
  number of bytes written (Linux returns the kernel mask size, 4 here, and
  requires `cpusetsize` ≥ 4, else EINVAL). Only the calling thread
  (`pid` 0 or own tid) is supported; another task → ESRCH if unknown,
  EPERM with detail otherwise. `setaffinity` with no online CPU in the mask
  → EINVAL.
- **`sched_setscheduler` [156], `sched_getparam`/`sched_setparam`.** Only
  `SCHED_OTHER` with priority 0 is accepted (success, already the state);
  `SCHED_FIFO`/`SCHED_RR` unprivileged → EPERM; others EINVAL. Keep
  `sched_getscheduler` as is.
- **`capget` [184], `capset` [185].** Report a consistent empty
  capability set for the ordinary user (versions 1, 2 and 3 headers;
  unknown version → EINVAL and write the preferred version back, as on
  Linux). `capset` succeeds only if the requested sets are subsets of the
  current (empty) sets; raising anything → EPERM.
- **`prctl` additions [172].** `PR_SET_DUMPABLE`/`PR_GET_DUMPABLE` (stored
  per process, 0/1 only), `PR_SET_NO_NEW_PRIVS`/`PR_GET_NO_NEW_PRIVS`
  (stored, one-way, inherited across fork and exec through the existing
  hidden-environment mechanism), `PR_SET_PDEATHSIG`/`PR_GET_PDEATHSIG`
  (store; delivery when the parent dies is not available on QNX without a
  watcher → store and report, and document that delivery is
  unsupported, *or* implement delivery if QNX offers a parent-death
  notification; builder checks).
- **`membarrier` [389].** `MEMBARRIER_CMD_QUERY` returns 0 (no commands
  supported); every other command returns EINVAL with detail
  `membarrier-cmd`. A barrier in the calling thread alone doesn't order
  other cores, so claiming `GLOBAL`/`PRIVATE_EXPEDITED` would be fake
  success; musl falls back to its correct signal-based method on failure.
  A real implementation (for example interrupting every guest thread) can
  come later if a workload needs it. (Designer's call, from the review.)
- **`setitimer` [104], `getitimer` [105].** `ITIMER_REAL` → QNX
  `setitimer`/`getitimer` (SIGALRM is mapped). `ITIMER_VIRTUAL`/`PROF` →
  QNX if supported, otherwise EINVAL with detail. 32-bit `timeval` layout.
- **`mremap` [163].** Only for anonymous private guest mappings tracked by
  `guest_memory.c`: shrink in place; grow in place if the next range is
  free; otherwise with `MREMAP_MAYMOVE` allocate, copy, and unmap the old
  range. File mappings, `MREMAP_FIXED` and `MREMAP_DONTUNMAP` → EINVAL
  with detail (musl's `realloc` falls back cleanly). Executable mappings
  must keep their patch/icache bookkeeping, so refuse them (EINVAL) rather
  than move them.

## Testing

- Each group adds direct ARM guest tests under `guest-tests/` (assembly for
  pure syscall checks, as `fd-close-all.S`; C fixtures built with
  `scripts/build-guest-fixture.sh` where threads or structs make assembly
  impractical). Tests use numbered exit codes so a failure names the check.
- Every test must fail on the pre-phase core (mutation-check the important
  ones, as Phase 1 did) and pass on the new one.
- Each group: build on the device, run its new tests plus the suites it
  touches (e.g. Group 4 → network smoke). End of phase: the full 14-suite
  regression (`ssh -tt`, 0 failures) and a corpus rerun
  (`scripts/run-corpus.sh`), with `docs/corpus-results.md` updated.
- Diagnostics: confirm through `LINUXEMU_DIAGNOSTICS` that the implemented
  calls no longer appear in the corpus reports.

## Delivery

- Git worktree on a `phase2-syscalls` branch, merged into master by
  fast-forward after the final review, as in Phase 1.
- One or more commits per group, each with the device evidence in the
  message (before/after exit codes), no `Claude-Session` trailers.
- `docs/compatibility.md`, `docs/development.md` (verified behaviour and
  limits) and the roadmap's Phase 2 status updated at the end. No release
  in this phase unless the user asks.

## Decided with the user

- Scope: roadmap Phase 2 list + its Android additions + the corpus-observed
  calls (except epoll/inotify, which stay Phase 3) + the Node fix.
- Approach A: one branch, five groups, Node first.
- Keep the phase order: no `epoll` in Phase 2 even though Node needs it to
  start. Node isn't the priority; record its next blocker and move on.
- Proper errors, never fake success.

## Calls made by the designer (the user can overrule)

- Unmapped signals are store-only; sending them to other processes is
  unsupported.
- `cacheflush` reuses the existing `msync(MS_INVALIDATE_ICACHE)` path and
  doesn't rescan JIT code for `svc`.
- Vector positional I/O and `sendfile64` are emulated with loops and are not
  atomic across iovecs.
- OFD locks approximate per-process record locks.
- `mremap` only moves anonymous, non-executable guest memory.
- `membarrier` reports no supported commands rather than a local-only
  barrier.
- `statfs64`/`fstatfs64` accept 84 and 88 bytes, as Linux does.
- The blind runtime-mapping patch scan is measured and recorded, and fixed
  in Phase 4.

## Left to the builder

- Exact task split inside each group and test file names.
- Where new code lives: new translation units (for example
  `src/linux_resource.c` for Group 5 and `src/linux_file_extra.c` for
  Groups 2–3) are preferred over growing `src/linux_syscall.c` past its
  current 1,900 lines; follow the existing file-per-area pattern.
- Whether `RWF_DSYNC`/`RWF_SYNC` are honoured with an `fsync`.
- The QNX source for `sysinfo` memory totals, and whether QNX offers a
  parent-death notification for `PR_SET_PDEATHSIG`.
