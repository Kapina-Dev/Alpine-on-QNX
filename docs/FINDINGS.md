# Findings

Log every device finding here: date, device, what was tried, result, and
evidence. When a finding disproves a roadmap assumption, also update
`docs/capability-roadmap.md`.

## 2026-11-16 — External research review (desk study; no device work)

**Sources:**
- stanw47/Blackberry-Classic-Research @ `6852b8a`
- stanw47/Blackberry-Passport-Research @ `24848b2` (session notes 48–52)
- bb10root Patreon: only the creator blurb and one teaser are public. The
  latest post, "v4 update 1" (Nov 9), adds an integrity check to the Android
  runtime launch script. The post body is paywalled (HTTP 403).

Neither repo has a license file. Use them as documentation only: reimplement
from the protocol facts below and don't copy their code.

The facts below come from those repos' authors, tested on retail Passport
and Classic devices. **Linuxemu has not reproduced any of them yet.**

### RIM's `/dev/binder` works from the Android player context

- `/dev/binder` is RIM's user-space resmgr, owned `1000:10011` with mode 660.
  - As devuser, `open` fails with EACCES. After a test-only `chmod 666` the
    open succeeds, but every devctl still fails with EACCES, because the
    driver checks credentials.
  - Running as the Android player (uid 1000 + abilities), Passport session 50
    measured:
    - `BINDER_VERSION` returns **7**;
    - `BINDER_WRITE_READ` is accepted; and
    - the transaction-memory setup works.
- Protocol facts, from Passport sessions 50–52 and Classic session 77:
  - The driver is reached with `devctl`, not `ioctl`:
    - `0xC0046209` = VERSION (4 bytes);
    - `0xC0186201` = WRITE_READ;
    - `0xC108620C` = CFG (0x108 bytes, with vm_size at +0x104); and
    - `0xC03C620B` = TXN (60 bytes).
  - SET_MAX_THREADS is accepted only in RIM's direction-swapped form
    `0x80046205`. The AOSP value `0x40046205` returns ENOSYS. The other
    BC/BR command words also use RIM's swapped direction bits.
  - **WRITE_READ is one devctl message:**
    `[binder_write_read][write stream + payloads][read area]`. The
    `write_buffer`/`read_buffer` fields and the transaction `buffer`/`offsets`
    fields are *offsets into that message*, which the driver reads with
    `resmgr_msgread`. They are not pointers.
  - Transaction memory: CFG creates `/dev/shmem/binder_<pid>` and peer-maps it
    into the client. It returns the client-side address at cfg+0x100. Calling
    `mmap()` on the binder fd is not supported.
  - The transaction struct uses the AOSP field order. RIM's 4.3 client sends a
    4-byte zero payload for an empty PING.
- Status in that project: the A11 libbinder runs `ProcessState::self()` and
  `defaultServiceManager()` in the player context. The first real
  transaction returned EINVAL, and was still unresolved at the last commit
  (2026-10-09).

### How to get code into the player context

- The container `/apps/sys.android.<id>.ns/native/` is mode 777 on the data
  partition, including `scripts/`.
- RIM's `android_launcher` runs
  `scripts/restart-android-core-wrapper.sh`, then `start-android-core.sh`.
  That script starts `lowmemorykiller`, `android_resmgr`, `epolld`, `logd`,
  `binder` and `shrimp`, and `waitfor`s each device node.
- A command prepended to the wrapper runs as the player.
- Triggers:
  - opening an Android app;
  - as root, the PPS messages `/pps/services/launcher/control`
    `start_system`/`stop_system`, plus a navigator invoke; and
  - `shrink:-20` written to `/dev/android/lowmemorykiller`, which kills the
    core.
- **Exec trust is per inode (fsecd)**, not POSIX mode or owner. New files fail
  with `mmap seg 0 failed errno=1`. Overwriting an existing trusted file in
  place (same inode) keeps the trust. That is how the probe and its libraries
  were run.
  - RIM's QNX-side binaries need the real `system/lib` libraries and the
    inherited `LD_LIBRARY_PATH`.
  - Overwriting `system/native/lib/libc.so` broke the core start.

### Privilege and gates

- `resmgr_attach` returns EPERM for devuser on every path (Classic session 79).
  RIM's binder attaches as uid 1000 *inside the player namespace* without
  calling any ability API.
  - So the gate depends on process identity or namespace, not on the path.
  - A resmgr of our own started from `start-android-core.sh` was staged as an
    A/B test (Classic session 65) but **never tested**.
- Retail root: the patched `btool` applies `pathtrust !/base/bin/__root`,
  triggered by `launcher_patcher` at boot. The "rooted" notification shows
  that it applied.
- Even as root:
  - setuid binaries can't be exec'd;
  - newly built PIEs are refused at exec;
  - writes to `/proc/<pid>/as` `.text` are blocked (errno 312); and
  - the boot partitions are hardware write-protected.
- Prototype-only root routes (an init keychord and CVE-2015-1805 on the "oslo"
  Android prototype) don't apply to retail devices.

### What this means for Linuxemu (hypotheses; verify on device)

1. **Binder Option A is now the leading path.** The driver works with player
   credentials, and Android 6's 32-bit libbinder already speaks protocol 7. So
   the translator needs:
   - the RIM command-word swap;
   - single-message packing with offsets; and
   - CFG-based receive memory.

   It doesn't need a 64↔32 conversion.
2. **Linuxemu must then run as the player.** Running as the ordinary user is
   not enough for binder.
   - The Linuxemu binary itself must pass fsecd trust in that context.
   - Linuxemu already copies guest executable segments into anonymous memory
     (`src/guest_memory.c`, the `copy_executable_file` branch of mmap in
     `src/linux_syscall.c`), so guest binaries don't need file trust.
   - Still unverified: whether anonymous W→X `mprotect` is allowed for the
     player.
3. **Caller identity:** the driver reports QNX credentials (uid 1000 for every
   process), not guest app UIDs. Android's `getCallingUid()` checks would see
   every app as `AID_SYSTEM`. The translator must rewrite `sender_euid` on
   receive, using a pid→guest-credential registry shared across Linuxemu
   processes.
4. **Coexisting with 4.3:** RIM's 4.3 `servicemanager` owns the context
   manager on the same driver. The Android 6 stack would replace RIM's 4.3
   userland (init, servicemanager, zygote) and keep RIM's QNX-side resmgrs.
5. **Conflict risk:** bb10root v4's integrity check on the Android runtime
   launch script may detect or undo a launch-script hook. Check before
   relying on that hook.

## 2026-11-17 — First device probes (Passport, devuser)

**Device:** Passport (LAN address omitted), QNX 8.0.0 build 2018/02/21. Run over SSH
as devuser: uid 101431000, gid 10143, groups 1000,1005,1001,750. The 4.3
runtime's nodes were present (created Nov 16 22:12).

### RIM binder probe (`native-probes/rim-binder-probe.c`)

- Built on the device with the SDK gcc (`-marm -Wall -Wextra -O2`), with no
  warnings.
- Device nodes:
  - `/dev/binder`: `1000:10011`, mode 0660. This matches the research.
  - `/dev/android/epoll`: `0:0`, mode **0777**, so devuser can open it.
  - `/dev/android/eventfd`: `android_system:10011`, mode 0660.
- `open("/dev/binder", O_RDWR)` returns **EACCES (13)** as devuser. Devuser
  is in group 1000 but not 10011. This reproduces the research result.
- Anonymous RW → RX `mprotect` and a call into the page: **PASS** (returned
  42) as devuser.
- **Still to do:** run the same binary from the Android player context via
  the core wrapper hook. That needs a launch-script edit and an in-place
  overwrite of a trusted inode. It hasn't been done yet.

### Static bionic probe (`guest-tests/bionic-probe.c`)

- Build: NDK r27c, `armv7a-linux-androideabi23-clang -marm -static -O2`,
  producing an ET_EXEC binary. SHA-256
  `4950f0262a3a0dfdd1764bd50241d9eb455a4ed7d12241244fd0ce65403fe7c8`.
- Even with `-marm`, the NDK's prebuilt `libc.a` is mostly Thumb:
  - 1261 `$t` mapping symbols against 38 `$a`;
  - 229 ARM `svc` against 4 Thumb `svc`; and
  - 103 Thumb-2 `mrc p15,0,rX,c13,c0,3` TLS reads against 2 ARM ones.
- Run with Linuxemu `build/linuxemu` (Oct 14 device build) and
  `LINUXEMU_SYSTRACE=1`:
  - `__ARM_NR_set_tls` (0xf0005) returned 0;
  - `getpid` returned the pid;
  - `mmap2` (temporary bionic TLS) returned `0x11006000`; and then
  - **SIGSEGV at pc `0x295d0`, fault address `0x4`**. No `main()` step
    ran.
- Root cause: `__libc_init_main_thread_late` reads TPIDRURO with the
  **Thumb-2** encoding `ee1d 0f70` at `0x3a004`.
  - Linuxemu only rewrites the ARM-mode form (`src/arm_patch.c`,
    `src/trap.c`). So the read returns the real register (0), not the value
    the guest set through `set_tls`.
  - Then tcb = 0 − 8, and `ldr [tcb+0xc]` faults at address 4.
- **Conclusion:** Linuxemu can't run any NDK/AOSP bionic code today. The
  first blocker is Thumb-2 TPIDRURO emulation (roadmap Phase 4), not
  binder. Patching the Thumb TLS reads is the smallest next step. Rerun this
  probe after that to find the next gap.

### Root attempt (same day, Passport)

- Root is reachable, but not by exec'ing `/bin/__root` from the laptop SSH
  session: that fails EPERM (sshd child isn't trusted to exec setuid). The
  route is a **second sshd on port 22**; from the devuser shell,
  `ssh -i ~/.ssh/id_rsa_bb10 -p 22 root@localhost` logs in as root.
- The **root shell only execs trusted (base-system) binaries.** `uname` runs;
  BerryCore-built binaries (`ls`, `id`, and our `rim-binder-probe`) are refused
  with "Operation not permitted" even by absolute path. clitools/berrycore are
  not on root's PATH, but PATH is not the cause — it is the per-inode fsecd
  trust gate on exec in elevated shells (matches the root-contract notes).
- So **root cannot run the binder probe as-is.** Running it as root would need
  the probe made trusted (overwrite a trusted inode in place) — destructive, not
  done. And root is uid 0, not uid 1000 inside the player namespace, so it
  probably would not pass the binder driver's credential check regardless. The
  Android player context remains the real unlock for binder, not root.

## 2026-11-17 — Thumb TLS/svc patching (Passport, devuser)

**Change:** Linuxemu now patches Thumb code as well as ARM.
- `src/thumb_decode.c` holds the pure logic:
  - it classifies 16- and 32-bit Thumb instructions and finds Thumb-2
    TPIDRURO reads (`ee1d Xf70`) and `svc #0` (`df00`), stepping one
    instruction at a time so the tail of a 32-bit instruction is never
    misread; and
  - it parses ARM mapping symbols (`$a`/`$t`/`$d`).
  - Host unit test: `tests/thumb-decode-test.c`, 34 checks, all pass.
- The static loader splits each executable section by mapping symbol:
  - ARM runs get the existing ARM patcher;
  - Thumb runs get `arm_patch_thumb_range`, which writes the 16-bit UDF
    `0xde00`; and
  - data runs are skipped.
  - A binary with no `.symtab` keeps the previous ARM-only behaviour.
- The trap handler advances the PC by the instruction width (2 or 4), and
  syscall restart backs up by the same width. Device results from 2026-09-22
  already showed that `0xde00` raises SIGILL, the saved PC is the exact
  halfword, and SPSR bit 5 marks Thumb state.

**Result:** `bionic-probe-armv7` now gets past `__init_bionic_tls_ptrs` and
runs bionic's libc init.
- 40 syscalls, 21 distinct, including a trapped Thumb `svc` at `0x1e58e`.
- It stops on ENOSYS from:
  - `personality` (136): **fatal**, because `__initialize_personality`
    aborts;
  - `sched_getscheduler` (157): warning only;
  - `prctl` (172); and
  - `rt_tgsigqueueinfo` (363), from the abort path.
- Bionic then calls `_exit(127)`, so `main()` still doesn't run.
- **Next blocker:** roadmap Phase 2 Android syscalls, starting with
  `personality`.

**Not covered yet:**
- Thumb patching for runtime `mmap` (`arm_patch_elf_mapping`, which matters
  for dynamic libraries);
- stripped binaries with no mapping symbols (they still need
  executable-segment decoding);
- Thumb signal handlers (bit 0 of the handler address → SPSR T bit); and
- Thumb entry points (still rejected).

**Regression (same day):** the full device regression passed with Thumb
patching: 14/14 suites, `linuxemu_full_regression_failures=0`.

## 2026-11-17 — Android start-up syscalls (Passport, devuser)

**Change:**
- New syscalls:
  - `personality`: per process; kept across `execve` through the hidden
    `LINUXEMU_GUEST_PERSONALITY` variable.
  - `sched_getscheduler`: always `SCHED_OTHER`, because guests can't change
    their policy yet; EINVAL for a negative pid, ESRCH for a missing task.
  - `prctl` `PR_SET_NAME`/`PR_GET_NAME`: per-thread name, inherited by new
    threads and set from the exec'd file's basename. `PR_SET_VMA` and
    unknown options return EINVAL.
  - `rt_tgsigqueueinfo`: to the calling thread with a handler installed,
    the guest's own siginfo is delivered on syscall return. Default and
    ignored actions, and other threads, use the tgkill path.
- `mmap` now accepts `MAP_NORESERVE` and maps it to QNX `MAP_LAZY`. On QNX,
  0x4000 is `MAP_NOINIT` (memory isn't zeroed), so the Linux flag must never
  be passed through.
- Tests:
  - `guest-tests/android-process-syscalls.S`: 22 checks, including the
    `execve` checks (it runs with `LINUXEMU_ROOT=/`, because `execve` needs
    a guest root). A mutant that drops personality forwarding fails with
    exit 21, as intended.
  - `memory-syscalls.S` gains a PROT_NONE + `MAP_NORESERVE` check that
    also verifies the memory is zero-filled. It was RED with exit 58 before
    the fix.
- The full device regression passed: 0 failures.

**Bionic probe progress:**
- `personality` was fixed, then bionic stopped on "executable's TLS segment
  is underaligned (8, needs 32)".
  - That was a toolchain mismatch, not an emulator bug: NDK r27's `libc.a`
    has gwp_asan ELF TLS, and only the API ≥ 29 `crtbegin_static.o` raises
    `PT_TLS` alignment to 32.
  - The probe is now built against `androideabi29` for the static crt. The
    libc.a code is identical.
- Next, `mmap(NULL, 0x5000, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|
  MAP_NORESERVE)` returned EINVAL. That's fixed (above).
- **Current blocker (roadmap Phase 6):** "32 bit bionic libc only accepts
  pid <= 65535, but current pid is 39047406".
  - 32-bit bionic packs thread ids into 16 bits of `pthread_mutex_t`, and
    Linuxemu passes QNX pids and tids through unchanged.
  - Guests need virtualised pids and tids ≤ 65535. That affects `getpid`,
    `gettid`, `kill`, `wait4`, fork return values, `si_pid` and `/proc`.
  - Recorded as a Phase 6 requirement. Work returns to roadmap phase order
    (Phase 0 next).

## 2026-11-17 — First structured diagnostics report (Passport, devuser)

Phase 0A diagnostics (`LINUXEMU_DIAGNOSTICS=<file>`) now write one line per
distinct unsupported call per process. `bionic-probe-armv7` (exit 134,
SIGABRT from bionic's pid ≤ 65535 check) reports:

```text
linuxemu-unsupported exe=build/guest-tests/bionic-probe-armv7 pid=171241710 nr=172 name=prctl error=EINVAL count=3 detail=prctl-set-vma args=0x53564d41,0x00000000,0x103af000,0x00004000,0x00015428,0x00000000
```

So before the pid abort, bionic start-up needs nothing else that Linuxemu
lacks. Its only unsupported call is naming anonymous mappings, which it
tolerates. The report was written because the self-sent SIGABRT now
flushes diagnostics first. After the final review, kernel and external
signals (SIGPIPE, SIGALRM, SIGTERM, ...) also write the report before the
process ends; only SIGKILL doesn't.

## 2026-11-17 — First application-corpus baseline (Passport, devuser)

`sh scripts/run-corpus.sh` scored all 11 roadmap apps (full results in
`docs/corpus-results.md`): **6 of 11 compatible** across all five stages,
and **7 of 11** after the deadlock fix below.
- **Compatible:** BusyBox (see below), apk, Python, Git, SQLite, Vim and
  GCC/make.
- **nginx and Redis** fail at basic because `epoll_create`/`epoll_create1`
  return ENOSYS (Phase 3).
- **inotify-tools** fails at basic because `inotify_init` returns ENOSYS
  (Phase 3).
- **Node.js** aborts at startup:
  - `node::ResetSignalHandlers` asserts that `sigaction(n, NULL, &old)`
    succeeds for every signal 1–64, as on Linux;
  - Linuxemu returns EINVAL when a Linux signal has no QNX mapping
    (`guest_signal_action`); and
  - the fix is to answer queries for every valid signal number.
- **BusyBox shutdown found a Linuxemu bug, now fixed:**
  - A fork child of busybox `sh` (`sleep 30 & kill -TERM $!`) deadlocked:
    its only thread sat in MUTEX at `0x14f64c4`. It happened in 2 of 10
    launches done the corpus way (as background jobs) with
    `LINUXEMU_DIAGNOSTICS` set, and 0 of 10 with it unset.
  - Cause: the Phase 0A diagnostics terminate handler called `getenv`,
    `pthread_kill` and `pthread_sigmask` from a signal handler. When SIGTERM
    arrived while the child was setting up its exec under libc's
    environment lock, the child deadlocked.
  - Fix: the report path is cached at start-up, a lock-free final flush is
    used in the signal handlers, and the handler re-raises with
    `sigprocmask` + `kill(getpid())`.
  - Smoke `diag-sigterm-race` went from 2/20 hangs to 0/20, and BusyBox now
    scores compatible.
- `statx` returns ENOSYS in every app (thousands of calls). The apps fall
  back, but it's the most frequent unsupported call.

**BerryCore's GNU `timeout` (coreutils 8.29) never fires on QNX:**
- `timeout 3 sleep 10` and `timeout -s 9 3 sleep 20` both run the full time
  and exit 0.
- A `wget` blocked on a stalled nginx socket stayed REPLY-blocked for
  minutes under `timeout -s 9 10`. A manual `kill -9` ended it at once, so it
  was killable.
- The corpus harness now uses its own watchdog (`corpus_timeout`).
- **The existing release regression scripts still use `timeout`,** so their
  hang guards have never worked on the device.
- **Regression during this work:** 13 of 14 suites passed.
  `run-python-smoke` failed with the known Phase 0 fault (`fatal signal=0xb
  pc=0x60067dc4 address=0`). An A/B run with `master`'s binary, which lacks
  the deadlock fix, fails identically, so it isn't caused by this work. It
  passed in every earlier full regression the same day.

## 2026-11-17 (later) — Device spike: reaching the Android player context

A throwaway feasibility spike on the Passport (QNX 8.0.0, as devuser),
aimed at the one make-or-break Android gate: does our code run in the
Android *player* context, where RIM's `/dev/binder` accepts credentials,
and is anonymous W→X memory allowed there? Both are prerequisites for
running any Android userland under Linuxemu. The spike did **not** reach
the player context; the reason is itself the finding.

### Proven as devuser (baseline, repeated)

- **W^X PASS.** `rim-binder-probe` mapped RW memory, `mprotect`'d it to RX,
  and called into it: returned 42. Anonymous W→X executable memory is
  allowed for the ordinary user. (Still unproven for the player uid
  specifically, but a strong positive signal for any sandbox we *can*
  enter, including a BAR app.)
- **binder EACCES** as devuser (`open("/dev/binder")` → errno 13), as the
  desk study predicted. `/dev/binder` is `1000:10011` mode 0660.
- Linuxemu runs guests here (exit-status, memory-syscalls) and reproduces
  the bionic `pid <= 65535` abort.

### Two ways into the player context, both blocked for an ordinary user

1. **Hooking `restart-android-core-wrapper.sh`** (the research's method:
   the container `scripts/` dir is 777, and a command prepended to the
   wrapper runs as the player when the launcher starts the core).
   - Tested across four reboots. **2 of 2 times the hooked wrapper was in
     place, root and the Android core failed to come up at boot** (the app
     reached the end of its loading bar and crashed; no `/dev/binder`, no
     `/tmp/android_core_started_by_player` marker, no probe output). **2 of
     2 times with the original wrapper restored, root applied and the core
     started** (once observed auto-starting at boot without opening an app).
   - Whether the edit is the cause or merely correlated, a hooked wrapper
     broke the runtime both times, so this route is not usable here.
     Restoring the original (byte-identical, same inode 9669) each time
     brought the runtime back.
   - Note on root timing: the one successful launch coincided with the
     "rooted" notification appearing. The core start appears tied to
     boot-time root patching, not to the wrapper.

2. **Overwriting a core-started binary in place** (keeps the inode's fsecd
   trust; `shrimp` is the ideal target — started last, backgrounded, and
   non-critical, after binder is already up; all core binaries under
   `native/system/bin/` are mode 777 `apps:10011` and writable by devuser,
   so this needs **no root**).
   - The in-place write (`cat probe > shrimp`, preserving inode 9866)
     failed with **`Resource busy`**: `shrimp` is held mapped by a running
     process. The write failed cleanly every time — `shrimp` stayed the
     untouched original.
   - **`slay shrimp` could not stop it:** from the devuser SSH shell, the
     player's processes are visible as `/proc/<pid>` entries but every
     `/proc/<pid>/as` read returns "Operation not permitted", and `slay`
     by name reports "Unable to find process". **The player runs in RIM's
     own namespace as uid 1000, and the ordinary-user shell is not in it.**
   - So the file can only be rewritten while its holder is stopped, and we
     cannot see or signal that holder. The core is up from boot (manual
     SSH always comes up long after), so there is no write window either.

### Verdict

The binder-from-player gate is **still unproven — not failed.** Entry into
the player context is gated by **namespace isolation** that an ordinary
user cannot cross:

- the wrapper route breaks the runtime on this device; and
- the in-place-binary route needs the core's holder stopped, which needs
  root acting *inside the player namespace* — and per the SSH/root notes,
  root here has a minimal trusted PATH and cannot exec our BerryCore-built
  probes as-is, and is uid 0, not uid 1000-in-namespace, so it likely
  fails the binder credential check too.

**Implication for the roadmap:** the Android track's first real dependency
is "run as the player" (a root/namespace problem), *before* any binder
translation, pid virtualisation or ART work. That is an infrastructure
gate, not an emulator gate. The emulator-side signals that *were* testable
(W^X, guest exec) are positive.

**Fully testable without any of this:** whether Linuxemu runs and flips
memory executable inside an ordinary BAR app sandbox — which is all the
"Linuxemu + its own terminal BAR app" goal needs. Today's devuser W^X PASS
is an encouraging early signal for that.

### Device left clean

`shrimp` and the wrapper were verified byte-identical to their originals
(same inodes) and the core healthy (`/dev/binder` present). The spike's
inert leftovers are confined to our own dirs: the probe binaries and
`shrimp-probe.c`/`shrimp-probe` (a variant of `rim-binder-probe` that
writes its report to a file), plus `shrimp.orig` and the wrapper backup
under the dev checkout's `build/`.

## 2026-11-17 — tmux attach freeze: QNX drops SCM_RIGHTS on a full receiver

Root cause of attached tmux stalling (and freezing Term49), found with
native QNX probes and confirmed in the emulator:

- **Native QNX loses descriptor-bearing AF_UNIX stream messages.** QNX's
  stream defaults are `SO_SNDBUF` 7168 and `SO_RCVBUF` 5120, so a sender can
  queue more unread data than the receiver holds. A `sendmsg` carrying
  `SCM_RIGHTS` that no longer fits the receiver is **dropped with its
  payload** while `sendmsg` reports success (the classic BSD
  `sbappendcontrol` failure path). Linux never loses it.
- Measured with `native-probes/unix-rights-pressure.c` variants: an fd frame
  sent behind ≤2000 unread bytes always arrives; behind 4000 bytes a 16-byte
  frame fits but a 1968-byte one is dropped; behind 5893 bytes the 16-byte
  frame is dropped. It doesn't depend on the signal handler, a tty fd,
  the control-buffer size or the record size. Draining first avoids it.
- The 2026-10-14 note that native tests "receive both descriptors and every
  payload" was wrong: parent and child shared one terminal and the output
  interleaved. With separate logs, native QNX loses the frame too.
- tmux's identify handshake is exactly this pattern: ~5.8 KiB of plain
  frames, then the terminal fds. The server never receives them and both
  ends wait forever.
- The earlier mitigation (raising the *sender's* `SO_SNDBUF` to 64 KiB) was
  the wrong side and made it worse (more unread backlog allowed).
- **Fix:** every AF_UNIX stream/seqpacket socket Linuxemu creates gets a
  256 KiB `SO_RCVBUF` (`socket`, `socketpair`; accepted sockets inherit it
  from the listener). Native confirmation: a 256 KiB receive buffer set on
  the listener delivered both fds in 9/9 runs, including a delayed receiver.
  The `SO_SNDBUF` adjustment was removed.
- **Evidence:** new guest test `unix-rights-backlog` (5893 unread bytes,
  then 16 bytes + an fd, drained with `MSG_DONTWAIT`): old core exit 124
  (frame lost) 3/3, fixed core exit 0 5/5. Real attached
  `tmux new-session` with `TERM=xterm-256color` in an SSH pty: fixed core
  attaches (`CLIENTS=1`), runs the pane and exits 0 (twice); old core hangs
  until a watchdog kills it (137). Full device regression on the fixed core:
  14/14, 0 failures.
- **Residual:** a receiver that leaves more than 256 KiB unread can still
  lose an fd frame (Linux would block the sender instead). Not yet
  confirmed on the physical Term49 terminal (tested over an SSH pty).

## 2026-11-17 — Phase 1: virtual file-descriptor layer (approach B)

Guest fd numbers stay native QNX numbers. A new record table
(`src/guest_fd.c`) attaches an optional shared, reference-counted object
to a number. Objects are identified by their backing fd's
`(st_dev, st_ino)`, measured unique per pipe on QNX and stable across
`dup` and `SCM_RIGHTS`. A record whose number was reused behind the
table's back is dropped when it's next used. It replaces the fixed
`fd_paths`, `directory_streams` and `eventfd_write_peers` arrays.

**Defects fixed, each with a guest test that failed first:**

- **Fds ≥ 256 lost their path:** `getdents`, `fchdir` and `*at` returned
  EBADF (`fd-path-records`, exit 131 → 0).
- **`dup` family on the wake eventfd** returned EOPNOTSUPP, and its write
  end used a guest-visible low fd (`wake-eventfd-dup`, 142 → 0). The write
  end now lives in the hidden range.
- **Records were lost across exec**, so inherited directory fds and
  eventfds broke. They're now carried in a hidden `LINUXEMU_FD_RECORDS`
  variable (`fd-exec-carry`, 2 → 0).
- **getdents' directory stream kept a guest-visible fd.** QNX has no
  `fdopendir`, so no stream is kept between calls: the saved position
  resumes a fresh one (`getdents-chunks`, 164 → 0).
- **A guest close-all loop could close emulator fds.** The guest's
  `RLIMIT_NOFILE` is now virtual, at the hidden base: **936** on the
  Passport, where the host limit is 1000/1000. close, dup2/dup3, F_DUPFD,
  poll and select refuse hidden numbers (`fd-close-all`, 151 → 0).
- **SCM_RIGHTS adoption, narrow scope.** A received fd whose pipe-backed
  object the receiver already holds (for example, inherited across fork)
  becomes that object again (`fd-rights-adopt`, 3 → 0). Adopting objects
  created after the fork, passed child → parent, or passed between
  unrelated processes is left to Phase 2/9.

**Pre-existing QNX behaviours found and fixed on the way:**

- **QNX `fork()` fails with EBADF** while another thread closes a
  descriptor. A native probe failed 1379 of 2000 forks under pipe/close
  churn. This was the cause of the intermittent `run-git-smoke` HTTPS
  failure: 1 in 5 runs on both the Task 3 and Task 4 cores. A failed fork
  creates no child, so Linuxemu retries (`fork-fd-churn`: about half the
  forks failed before, none after; git smoke 0/5 failures afterwards).
- **QNX `fcntl(F_SETFD)` returns the previous flags.** Linux returns 0,
  and guests clearing close-on-exec got 1 back (`fd-path-records`
  double-set check, 136 → 0). `F_SETFD` and `F_SETFL` now return 0.

**Gates:** the stress fixture `fd-stress` covers dup/replace (1000
rounds), fork (200), fork+exec (50) and 4 threads × 2000 racing
`dup`/`close`. The open-fd count must equal the baseline after each phase.
- **Final results on the Passport:**
  - full regression: 0 failures;
  - every new guest test: exit 0;
  - corpus: 7/11 compatible, with every app's stages and
    unsupported-call counts unchanged from the baseline;
  - `fd-stress`: 0 failures in 5 runs. On the Task 3 core it fails with
    exit 5, so the gate catches lost exec carry-over.
- **Known limitations, by design:**
  - Linuxemu's own fds are hidden by number range and refused by the
    guards. They are not truly invisible.
  - Closing an fd while another thread polls it behaves as on QNX, not as
    on Linux.
  - `select()` doesn't translate a typed fd's `poll_fd` yet. No object
    needs it until Phase 3.

## 2026-11-18 — Phase 2: runtime-mapping patch hazard

`linux_mprotect_runtime` runs `arm_patch_range` over the whole range each
time an anonymous runtime mapping becomes executable. It can't tell code
from data, so it may rewrite data words. `guest-tests/jit-patch-hazard.S`
measures this on the Passport (not part of any regression runner):

- **Data corruption: present.** A RW anonymous page holding the words
  `0xef000000` (`svc #0`) and `0xee1d0f70` (a TPIDRURO read) was flipped RX
  with `mprotect`. At least one word no longer read back intact (the test
  prints `data-changed`). A JIT literal pool with such values would be
  silently turned into traps.
- **Patch-table exhaustion: present.** A loop of RW→RX `mprotect` flips over
  one page containing a single `svc` word fails at iteration 4087 of 5000
  (the same on two runs). The patch table fills (presumably because every flip records
  a new patch and none are released; not confirmed in the source). A JIT that recycles code pages would
  hit `mprotect` failures after about 4000 flips.

The test exits 21 (the loop failure takes precedence over exit 20 when both
occur). Both results were measured on the new core, with `cacheflush`
implemented, so `cacheflush` doesn't change them: it never patches.

**Hand-off to Phase 4:** the fix is to patch only real instructions, which
needs code/data knowledge Linuxemu doesn't have for JIT memory. It belongs
with Phase 4's "Thumb and stripped-binary support" patcher work. Until then,
measure Node with `--jitless` if it reaches its JIT and fails because of
this.
