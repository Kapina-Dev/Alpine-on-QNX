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
  Passport, where the host limit is 1000/1000 (744 since Phase 3 part 1
  raised the hidden reserve from 64 to 256). close, dup2/dup3, F_DUPFD,
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
- **Patch-table exhaustion: fixed.** A loop of RW→RX `mprotect` flips over
  one page containing a single `svc` word failed at iteration 4087 of 5000
  (the same on three runs). The cause, confirmed in the source:
  `arm_patch_range` appended a new site for every flip of the rewritten
  `svc` and never looked for the existing one, so the 4096-entry table
  filled and `arm_patch_lookup` kept returning the first, stale entry. Now
  a site at an address already recorded is updated in place. The loop
  completes all 5000 flips (3 of 3 runs; the test now exits 20, for the
  data corruption only).

Before the fix the test exited 21 (the loop failure takes precedence over
exit 20 when both occur). Both results were measured with `cacheflush`
implemented, so `cacheflush` doesn't change them: it never patches.

**Hand-off to Phase 4:** the data corruption remains. The fix is to patch
only real instructions, which
needs code/data knowledge Linuxemu doesn't have for JIT memory. It belongs
with Phase 4's "Thumb and stripped-binary support" patcher work. Until then,
measure Node with `--jitless` if it reaches its JIT and fails because of
this.

## 2026-11-18 — Phase 2: QNX facts found while adding syscalls

Found on the Passport as the ordinary user. Each has a guest test or a note
in the code.

- **`dup2` is not atomic against a concurrent `close`.** Racing `dup3` and
  `close` threads produced `EBADF` and `EINVAL`. Linuxemu now takes a
  process-wide lock around `dup2`, `dup3` and `close`, and resets it in the
  fork child.
- **`off_t` is 32 bits.** The core was truncating 64-bit offsets in vector
  I/O, `pread64`, `pwrite64`, `ftruncate64`, `_llseek`, `stat64` sizes and
  `mmap2`. These now use the `*64` calls, and so do `fcntl` record locks
  (`F_GETLK64`/`F_SETLK64`/`F_SETLKW64`) and resource limits
  (`getrlimit64`/`setrlimit64`). With 32-bit `off_t`, `rlim_t` is 32 bits
  and `RLIM_INFINITY` is `0x7fffffff`.
- **No file size cap; the cap was the open flag.** An earlier note said
  qnx6 and exFAT stop a file near 2 GiB. A native probe on qnx6 showed the
  cause: a descriptor opened without `O_LARGEFILE` gets `EFBIG` for a
  write at 3 GiB and `EOVERFLOW` for a seek past 2 GiB. With `O_LARGEFILE`,
  `pwrite64` of one byte at 3 GiB and at 4 GiB + 4096 succeeded, and
  `fstat64` and `pread64` read them back. Linuxemu now opens every guest
  file with `O_LARGEFILE`. The exFAT card was not re-measured.
- **qnx6 is not sparse.** The 3 GiB probe file used 3 GiB of blocks and took
  minutes to write. Even a zero-length `pwrite64` at 4 GiB extends the file
  and zero-fills it, so Linuxemu sends a zero-length `pwrite64` to QNX at
  offset 0.
- **QNX `pwrite` ignores `O_APPEND`.** It writes at the given offset, so
  `pwritev2(RWF_NOAPPEND)` is a plain positioned write.
- **No finite `RLIMIT_FSIZE`.** `setrlimit64(RLIMIT_FSIZE, ...)` returns
  `EINVAL` for every finite value tried (4 KiB to 4 GiB) as the ordinary
  user. `RLIMIT_DATA` and `RLIMIT_CORE` take 4 GiB.
- **Pipes hold 5120 bytes** (`F_GETPIPE_SZ` reports it).
- **`F_SETOWN` and `F_GETOWN` return `ENOSYS`.** Linuxemu answers `EINVAL`.
- **`sysconf(_SC_PHYS_PAGES)` is -1.** `sysinfo` reads total RAM from the
  system page and free RAM from typed memory.
- **`getpriority` and `setpriority` are stubs** that always return 0. Nice
  values are emulated in Linuxemu and carried across `exec`.
- **No parent-death notification.** `PR_SET_PDEATHSIG` is stored and
  reported, never delivered.
- **Run masks.** `_NTO_TCTL_RUNMASK_GET_AND_SET` does not write back the old
  mask; the `_INHERIT` form is needed for `sched_getaffinity`.
- **Anonymous mappings that are read-execute fail with `ENOMEM`**, so
  `mremap` and JIT paths avoid that combination.
- **`ITIMER_VIRTUAL` is `EINVAL`**, and an armed `ITIMER_REAL` doesn't
  survive `exec`.
- **`stat` timestamps are whole seconds**, so `statx` has no nanoseconds.
- **`run-python-smoke` faults.** The phase-gate run hit a python3 SIGSEGV at
  pc `0x60067dc4` in 4 of 4 runs on the new core, and on the pre-phase core
  too. It's a pre-existing fault, recorded in `docs/corpus-results.md` and
  not fixed here.

## 2026-11-19 — Phase 3 part 1: device probes for eventfd and epoll

Passport, ordinary user. Probes: `native-probes/phase3-probes.c` and
`native-probes/probe_epolld.c` (built by `scripts/build-native-probes.sh`).
Raw output of one run of each, back to back (rerun at the Phase 3 part 1
gate so that every line comes from the same run; the verdicts match the
first runs):

```
fifo_note=mkdir /tmp/p3fifo507519170 failed errno=2
fifo_rdwr.tmp=fail:2 (step 2)
fifo_rdwr.disk=ok
fifo_rdwr=ok-disk-only
fifo_rdwr.step=2 (the /tmp failure)
dlsym_control=pthread_mutexattr_setpshared found
dead_owner_plain=ETIMEDOUT
robust_header=PTHREAD_MUTEX_ROBUST not defined
dlsym_setrobust=missing dlsym_consistent=missing
robust_mutex=absent (via dlsym: setrobust missing, consistent missing)
race_note=new pipe reused fd 4 and was written
poll_close_race=n=0 revents=0x0 (POLLIN=0x5 POLLNVAL=0x1000) after 2002ms (blocked until timeout)
rdhup_signal.tcp=n=1 revents=0x1 (POLLIN=y POLLHUP=n POLLRDNORM=y) peek=0 errno=0
rdhup_signal.unix=n=1 revents=0x1 (POLLIN=y POLLHUP=n POLLRDNORM=y) peek=0 errno=0
stat./dev/android/epoll=uid=0(root) gid=0(nto) mode=777
stat./dev/binder=uid=1000(?) gid=10011(?) mode=660
open./dev/android/epoll=ok fd=3
epoll_ctl_devctl=not attempted (protocol recovery out of scope)
```

- **`fifo_rdwr=ok-disk-only`.** `/tmp` is a symlink to `/dev/shmem`, which
  refuses directories (`mkdir` gives `ENOENT`) and FIFOs (`mkfifo` gives
  `ENOENT`, step 2). On the data filesystem (`/accounts/1000/shared/misc`)
  `mkfifo`, `open(O_RDWR|O_NONBLOCK)` and `unlink` work; a written byte makes
  `poll` report `POLLIN`, a read clears it (`poll` with timeout 0 reports
  nothing), and a forked child's write wakes the parent's `poll`. A FIFO
  channel therefore needs a directory on a disk filesystem, not `/tmp`.
- **`robust_mutex=absent`.** The probe's positive control passes
  (`dlsym_control=pthread_mutexattr_setpshared found`, so `dlsym(RTLD_DEFAULT)`
  does search libc), `PTHREAD_MUTEX_ROBUST` is not defined by the SDK
  headers (`robust_header=... not defined`), and `dlsym` finds neither
  `pthread_mutexattr_setrobust(_np)` nor `pthread_mutex_consistent(_np)`.
  What a dead owner does instead: a `PTHREAD_PROCESS_SHARED` mutex in shm
  whose holder `_exit`ed without unlocking was still locked 1 s later
  (`dead_owner_plain=ETIMEDOUT` from a 1 s `pthread_mutex_timedlock`; the
  probe waits no longer than that), so shared state can't rely on a lock
  surviving its owner.
- **`poll_close_race`: `poll` is not woken by a close from another thread.**
  Thread A polling a pipe read end for 2 s was still blocked at the timeout
  (`n=0 revents=0x0` after 2002 ms) although thread B had closed that fd
  after 100 ms and a new pipe reused the number and was written. No
  `POLLNVAL`, and the new pipe's `POLLIN` isn't seen either: the poll keeps
  waiting on the old object.
- **`rdhup_signal`: no `POLLHUP` or `POLLRDHUP`-like bit on half close.**
  After the peer's `shutdown(SHUT_WR)`, a loopback TCP socket and an
  `AF_UNIX` stream socket both report `revents=0x1` for a poll mask of
  `POLLIN|POLLHUP|POLLRDNORM`. `POLLHUP` is clear. On QNX `POLLIN` is 0x5, so
  `revents=0x1` is the `POLLRDNORM` bit alone (the probe's "POLLIN=y" is a
  partial-bit match). `recv(MSG_PEEK)` returns 0, so peer
  shutdown is detected as readable plus a zero-length peek.
- **shm facts (earlier throwaway probe, same day).** As the ordinary user,
  `shm_open` + immediate `shm_unlink` + `ftruncate` + `mmap(MAP_SHARED)` work
  and leave no name in `/dev/shmem`. A `PTHREAD_PROCESS_SHARED` mutex and
  condition variable in that memory work across `fork`. The shm fd passes
  over `SCM_RIGHTS` and the receiver's mapping shares the counter.
- **`/dev/android/epoll`.** With the Android runtime's nodes present
  (`/dev/android/eventfd` dated today, `/dev/binder` and `epoll` dated
  2026-11-18, `/tmp/android_core_started_by_player` present; the ordinary
  user can't read the process list, so the processes themselves weren't
  inspected), `/dev/android/epoll` is owned by root:nto with mode 777 and
  `open(O_RDWR)` **succeeds** as the ordinary user, unlike `/dev/binder`
  (owner 1000, group 10011, mode 660, `EACCES` in the 2026-11-17 spike).
  The `epoll_ctl` devctl was not attempted: identifying it needs protocol
  recovery, which is out of scope.

## 2026-11-19 — Phase 3 part 1: sockets share one (st_dev, st_ino)

Passport, ordinary user, native probes. Each was a throwaway file built with
the SDK gcc and not committed. **Disproves an assumption of the epoll
design:** that the (st_dev, st_ino) recorded at `EPOLL_CTL_ADD` identifies
the file behind an fd number.

**Probe 1 (identity).** It creates two pipes, two `AF_UNIX` socketpairs,
and a loopback TCP listener with two clients and two accepted sockets. It
prints `fstat64`, `iofdinfo(_FDINFO_FLAG_LOCALPATH)` and
`ConnectServerInfo` for each fd. Then it closes one pipe, one socketpair
end, one client and one accepted socket, and creates another of the same
kind on the freed number. Output, trimmed to the identity fields:

```
pipe1 r/w         fd=3,4   dev=0x1 ino=0x10655430 mode=010666
pipe2 r/w         fd=5,6   dev=0x1 ino=0x107b8738 mode=010666
spair1 a/b        fd=7,8   dev=0x1 ino=0x30503523 mode=0140000  fdinfo path=/dev/socket/1, /dev/socket/2
spair2 a/b        fd=9,10  dev=0x1 ino=0x30503523 mode=0140000  fdinfo path=/dev/socket/2
tcp listen        fd=11    dev=0x1 ino=0x3050385b mode=0140000  fdinfo path=/dev/socket/2
tcp client1/2     fd=12,13 dev=0x1 ino=0x3050385b
tcp accepted1/2   fd=14,15 dev=0x1 ino=0x3050385b ctime=0
pipe reused num   fd=3     dev=0x1 ino=0x107b8690   (differs from the closed pipe)
spair reused num  fd=3     dev=0x1 ino=0x30503523
tcp reused num    fd=3     dev=0x1 ino=0x3050385b
tcp accept reused fd=7     dev=0x1 ino=0x3050385b
all sockets: rdev=0x0 size=0 nlink=0; fdinfo mode=0140666 ioflag=0x3
  offset=0 size=0; server pid=3706951 chid=2 scoid=1073741934 (coid = fd)
```

What it shows:

- **Pipes:** each pipe has its own `st_ino`, shared by its two ends. A new
  pipe on a reused number gets a different one.
- **Sockets:** every socket of one family has the same (st_dev, st_ino):
  - all `AF_UNIX` sockets are `0x1/0x30503523`;
  - all TCP sockets (listening, client and accepted) are `0x1/0x3050385b`.
- **Nothing else tells two sockets apart.** That covers `st_rdev`,
  `st_size`, `st_nlink`, the `iofdinfo` fields (path is only
  `/dev/socket/N`) and the server connection info.

So a stale epoll item can't be found by identity when one socket replaces
another on the same number.

Linuxemu now gives every guest fd number an epoch, kept in `guest_fd.c`.
It is bumped whenever the number is closed, replaced by dup2/dup3, or
handed out again. Each epoll item records its number's epoch at ADD, and
(st_dev, st_ino) stays as a second check. The `epoll-concurrent` fixture's
check 33 pins this case. The Task 5 core, which used identity alone, fails
it (exit 33); the Task 6 core passes it.

**Probe 2 (close of a polled socket).** Thread A polls an accepted TCP
socket and an idle pipe. Thread B closes the socket after 50 ms, accepts a
pending connection onto the same number and writes to it. Output:

```
round 0 blocked poll: n=1 revents fd6=0x41 x=0x0
closer: accepted fd=6 (old 6)
round 0 fresh poll: n=1 revents fd6=0x1 x=0x0
```

So `poll()` **is** woken when another thread closes a *socket* it watches.
It reports `POLLRDNORM|POLLHUP` (0x41) as the close happens, before the
accept. A pipe behaves the other way: the `poll_close_race` result above
shows no wake at all. A poll that starts after the replacement sees the new
socket (0x1).

For epoll, a closed watched socket therefore makes the waiter look at the
number at once. That number may already name the new socket, so the close
hook and the epoch must retire the item before the native close.

## 2026-11-19 — Phase 3 part 1: QNX poll() shows no hangup at pipe EOF

Passport, ordinary user, a native probe built with the SDK gcc (throwaway,
not committed). It polls a pipe's read end with `POLLIN|POLLRDNORM` and
with events 0, reads `FIONREAD` and `fstat`, at each step: open and empty,
data written, writer closed with data left, data read, and a pipe whose
writer closed before any write. Then a write end whose reader closed, and
an `AF_UNIX` stream socket after the peer's `shutdown(SHUT_WR)`. Output
(QNX bits: `POLLRDNORM` 0x1, `POLLWRNORM` 0x2, `POLLIN` 0x5, `POLLERR`
0x20, `POLLHUP` 0x40):

```
open-empty:           poll=0 revents=0x0 fionread=0 fifo=1 mode=010666
open-data:            poll=1 revents=0x1 fionread=2
closed-data:          poll=1 revents=0x1 fionread=2   (writer closed)
closed-empty:         poll=1 revents=0x1 fionread=0   (data read; read() then returns 0)
never-written-closed: poll=1 revents=0x1 fionread=0
  (events=0 on each of these: poll=0 revents=0x0)
writer-reader-gone:   poll=1 revents=0x2              (no POLLERR)
sock-shutwr:          poll=1 revents=0x1 fionread=0 fifo=0 sock=1 mode=0140000
sock peek (1 byte, MSG_PEEK|MSG_DONTWAIT) = 0
```

What it shows:

- **QNX never reports `POLLHUP` on a pipe's read end** when the last
  writer closes, with or without data left, and not even with events 0.
  It reports only readable. Linux reports `POLLHUP`, and `POLLIN` only
  while data remains.
- **An empty pipe at EOF is the only empty pipe QNX calls readable**
  (`FIONREAD` 0), so that state can be recognised. With data left, nothing
  shows that the writer is gone.
- A write end whose reader closed shows only `POLLWRNORM`; Linux adds
  `POLLERR`. Not handled.
- A socket after `SHUT_WR` looks the same as a pipe at EOF (readable,
  `FIONREAD` 0), so the EOF rule is limited to `S_ISFIFO`.

Linuxemu now reports a pipe read end that QNX calls readable with
`FIONREAD` 0 as `POLLHUP` without `POLLIN` (`linux_poll_pipe_hangup`), in
`poll`/`ppoll` and in epoll (`EPOLLHUP`), as Linux does for an empty pipe
with no writers. Known limit: with unread data left, the hangup shows only
once the data is read. The `epoll-modes` fixture's check 18 pins this.

## 2026-11-19 — Phase 3 part 1: other QNX facts found while adding eventfd and epoll

Passport, ordinary user, native probes and builds (throwaway probes, not
committed). Each fact is pinned by a guest test or a note in the code.

- **No inline 64-bit compare-and-swap with the SDK compiler.** GCC 4.6.3
  (`-march=armv7-a`) compiles `__sync_val_compare_and_swap` on a
  `uint64_t` to a call to `__sync_val_compare_and_swap_8`, which the QNX
  `libgcc.a` doesn't have, so the link fails:

  ```
  00000000 <cas64>:  0: eafffffe  b  0 <__sync_val_compare_and_swap_8>
  cas64.c:(.text+0x0): undefined reference to `__sync_val_compare_and_swap_8'
  ```

  The eventfd counter therefore uses an inline `ldrexd`/`strexd` loop
  between `dmb` barriers (the Linux kernel's `atomic64_cmpxchg` sequence).
  `nm` of the core lists no `__sync*` or libatomic symbol. The 32-bit
  `__sync` builtins are inline (`ldrex`/`strex`).
- **`POLLHUP` on a full peer close.** QNX does report a stream socket's
  hangup once the peer has closed completely, but not on a half close or on
  pipes. QNX bits: `POLLRDNORM` 0x1, `POLLWRNORM` 0x2, `POLLHUP` 0x40,
  `POLLNVAL` 0x1000.

  | Case | revents | `recv(MSG_PEEK)` |
  | --- | --- | --- |
  | stream socketpair, open | 0 | -1 `EAGAIN` |
  | peer fully closed, events `POLLIN` | 0x41 | 0 |
  | peer fully closed, events 0 | 0x40 | 0 |
  | peer fully closed, events `POLLIN\|POLLOUT` | 0x43 | 0 |
  | peer wrote 1 byte, then closed | 0x41 | 1 |
  | peer `shutdown(SHUT_WR)` only | 0x1 | 0 |
  | both sides `SHUT_WR` | 0x43 | 0 |
  | datagram socketpair, peer closed | 0x1 | -1, errno 254 |
  | pipe read end, writer closed | 0x1 | — |
  | pipe write end, reader closed | 0x2 | — |
  | closed fd in the set | 0x1000 | — |

  So `EPOLLHUP` on a closed stream peer needs no emulation. Pipes are
  covered by the pipe EOF entry above.
- **A pipe's write end reports `POLLRDNORM` while the pipe holds data.**
  Linux never reports `POLLIN` on a write end. `epoll-basic` registers
  write ends with events 0 so its checks don't depend on it.
- **QNX `select()` works above its `FD_SETSIZE` of 256.** With a large
  enough set, a pipe at fd 800 is reported correctly, empty (0) and with
  data (1). Hiding the per-thread signal-wait pipe put it at 744 or above,
  where `pselect6` had refused it (`EMFILE`, `signal-queue-wait-syscalls`
  exit 8). Linuxemu's host fd sets are now 4096 bits.
- **`msync(MS_ASYNC)` tells mapped from unmapped memory.** An unmapped page
  or NULL gives `ENOMEM`; bss, stack, text and `mmap` pages give 0.
  `epoll_wait` uses it as the `EFAULT` fallback for guest memory the
  loader doesn't track. It shows that a page is mapped, not that it is
  writable.
- **A `poll()` busy loop costs less CPU than it seems.** A wait spinning
  on a pipe for 200 ms used only 50 to 70 ms of
  `CLOCK_THREAD_CPUTIME_ID`, because most of each round trip is QNX's pipe
  manager (another process) answering the poll; a sleeping wait uses 0 to
  1 ms. The `epoll-nesting` spin detector (check 17) uses a 20 ms
  threshold for that reason.

## 2026-11-19 — io-pkt crashed under the signal-wait channel stress test

Passport, ordinary user, branch `phase3a-events` at 265f24f. The
signal-wait channel was then a self-connected loopback UDP socket per
thread, written by the emulator's host signal handler during masked waits.
An outside report on a Go-on-QNX port said io-pkt misbehaves when
Unix-domain socket operations (bind, unlink, `SCM_RIGHTS`) are interrupted
by signals, so `guest-tests/iopkt-signal-stress.c` was written to load
those windows: 8 waiter threads in masked `ppoll`/`epoll_pwait` and blocking
eventfd reads, AF_UNIX stream ping-pong passing a pipe fd by `SCM_RIGHTS`
every message, TCP loopback ping-pong, short-lived threads that do a masked
`ppoll` and an AF_UNIX bind/unlink and exit, an AF_UNIX path
bind/listen/close/unlink loop, and a signaller sending `tgkill` to every
live thread about every 100 us.

- The first run (10 s, `build/linuxemu`, socket channel) printed nothing.
  At 16:32:16 GMT the system log recorded `IopktMonRun: io-pkt process
  dies` and `io-pkt crashed possibly`, and left
  `/var/log/io-pkt-v6-hc.core`. Wi-Fi never came back (ssh and ping
  failed). The phone was shut down with the power key from the menu at
  16:37:24 and booted again at 16:39.
- Not thermal: `limit_device_to_shutdown:0`, `wifi_temp_nominal:1`, and the
  PON reasons showed no thermal cause.
- An ordinary user can take down the whole network stack this way; the
  network stays down until reboot.
- **Trigger not yet isolated.** The test combined the UDP channel written
  from signal handlers (io-pkt messages sent from a handler while the
  same thread may be inside its own io-pkt call) with AF_UNIX
  `SCM_RIGHTS` and path bind/unlink under constant signals. Which part
  does it is unknown.
- Action: the signal-wait channel is a persistent hidden pipe again
  (pipe-manager, not io-pkt), so a signal handler never sends io-pkt a
  message. The concurrently-waiting thread limit returns to about 127.
  The stress fixture and `scripts/run-iopkt-stress.sh` are kept for
  isolating the trigger and are not part of the regression; run them only
  with someone at the phone to reboot it.
- The same test passed natively on Linux (x86, 8 s, all classes
  progressed, no integrity failures).

## 2026-11-19 — Phase 3 part 2: device probes for timerfd and signalfd

Passport (QNX 8.0.0), ordinary user. Probe: `native-probes/phase3b-probes.c`
(built by `scripts/build-native-probes.sh`, bounded by `alarm(60)`; no signal
handler touches io-pkt). Raw output of the final run (every value below is
**measured**; earlier runs of the first probe version agreed on the
sigwait, accuracy and clock verdicts):

```
sigwait_collect=ok
# sigwait handler_ran=0 err=0,0,0
sigwait_siginfo=kill(pid=54194178,uid=0,code=0,value=0);sigqueue(pid=54194178,uid=0,code=-2,value=42);child_kill(pid=54198479,uid=0,code=0,value=0)
# expected: self pid=54194178 uid=101431000, child pid=54198479; SI_USER=0 SI_QUEUE=-2
pulse_timer_accuracy=2550,5124 (monotonic relative median_us,p99_us; realtime absolute 1546,3257; lateness vs ideal, 200 x 10 ms)
pulse_coalescing=queued,tick_limited (100us/50ms=50 of 500; 100us/100ms=100 of 1000; 5ms/50ms=10 of 10; overrun=0,0,0)
monotonic_in_sleep=unknown
# monotonic_in_sleep cannot force screen-off sleep unattended
# clock_settime(REALTIME)=fail errno=1; settimeofday errno=1; uid=101431000; offset drift across calls ns=0
# procmgr_event_notify_add(PROCMGR_EVENT_TOD) = 1 errno=0
# spurious TOD pulse after registration: 0
# procmgr_event_trigger(TOD)=0 errno=0, pulse delivered=1
realtime_change_detect=procmgr_event_tod_unverified (registration ok, only a manually triggered pulse delivered; no real clock change possible, clock_settime errno=1)
```

- **`sigwait_collect=ok`.** With SIGUSR1 blocked in every thread (main, two
  idle threads, the waiter) and collected by the waiting thread
  (`sigtimedwait`, 1 s limit), a process-directed `kill`, a `sigqueue` and a
  fork child's `kill` were all taken by it, and the installed handler never
  ran.
- **`sigwait_siginfo`.** `si_pid` is the sender's QNX pid (correct for self
  and child), `si_code` is `SI_USER` (0) for `kill` and `SI_QUEUE` (-2) for
  `sigqueue`, and `si_value` carries 42. **`si_uid` is 0 in all three cases**
  although the sender's uid is 101431000: the sender uid is not reported
  by `sigwaitinfo`/`sigtimedwait`. signalfd `ssi_uid` cannot be filled from
  it.
- **`pulse_timer_accuracy`** (median, p99 lateness in microseconds, 200
  pulses of a 10 ms periodic `SIGEV_PULSE` timer): `CLOCK_MONOTONIC`
  relative 2550/5124; `CLOCK_REALTIME` absolute 1546/3257. Lateness is
  millisecond-scale (an earlier run gave 2415/5626 and 705/3903). Minimum
  and maximum lateness were not printed.
- **`pulse_coalescing=queued,tick_limited`.** Three runs separate a tick
  limit from a queue cap: a 100 us periodic timer over a 50 ms sleep gave
  50 pulses and over a 100 ms sleep gave 100 (a 50-entry cap would give 50
  both times), so the rate is about one pulse per 1 ms; a 5 ms timer over
  50 ms gave 10 of 10 (queued, not merged). `timer_getoverrun` was 0 in all
  three. Pulse count cannot give the expiration count for sub-tick periods;
  compute expirations from the clock.
- **`monotonic_in_sleep=unknown`.** A screen-off device sleep cannot be
  forced unattended; not measured.
- **`realtime_change_detect=procmgr_event_tod_unverified`.** As an ordinary
  user `clock_settime` and `settimeofday` fail with `EPERM` (errno 1), so a
  real clock change could not be produced. The SDK header `sys/procmgr.h`
  defines `PROCMGR_EVENT_TOD` ("time-of-day changed");
  `procmgr_event_notify_add` with a pulse event is accepted (returns id 1),
  no spurious pulse arrives, and `procmgr_event_trigger(PROCMGR_EVENT_TOD)`
  delivers the pulse. Registration and delivery of a manually triggered
  pulse are verified; delivery on a real clock change is not. A grep of
  `time.h`, `sys/neutrino.h` and `sys/procmgr.h` in the SDK for clock-change
  and time-change names found no other notification mechanism.

## 2026-11-19 — Phase 3 part 2: signal masks in handlers and real-time queueing (signalfd task)

Passport (QNX 8.0.0), ordinary user. Native probes written while building
signalfd (Task 4); not committed (one-off `pthread_sigmask`/`sigqueue`
programs).

- **A mask change inside a signal handler does not survive the handler's
  return.** Neither `pthread_sigmask` in the handler nor editing the
  handler's `ucontext_t` `uc_sigmask` (which QNX leaves zeroed) changes the
  mask after return: 0 of 4 combinations kept SIGUSR1 blocked.
  `SignalReturn` restores the mask saved in the frame QNX builds for the
  handler, `struct _sighandler_info` (`sys/neutrino.h`): the saved
  `sigset_t` is the start of `data[]`, right after `siginfo_t`, `handler`
  and `context` (offset 48). Writing there does persist (USR1 blocked
  after return = 1).
- **Consequence for Linuxemu:** syscalls run inside the SIGILL trap handler,
  so a guest `sigprocmask` never reached the host thread. A guest that
  blocked a signal and then received it either had the host handler queue
  it (guest handler installed) or was killed by the default action:
  `block SIGUSR1; kill(getpid(), SIGUSR1)` ended the process with SIGUSR1
  on the pre-Task-4 core (exit 144, with and without a second thread). The
  signalfd design assumed the host mask followed the guest's.
- **Fix (Task 4):** after `rt_sigprocmask` and `sigreturn` the trap writes
  the guest's mask into its own frame's saved mask
  (`guest_signal_apply_frame_mask`). A start-up self-check
  (`guest_signal_frame_probe`: a handler on SIGEMT must find a known mask
  at that offset, and SIGUSR2 written into it there must be blocked after
  return) enables it; if the layout ever differs the old behaviour stays
  and `LINUXEMU_DIAGNOSTICS` reports `signal-frame-mask`. Because the host
  mask now follows the guest's, `execve` (run inside the trap) passes the
  blocked set on, as Linux does, and the new image seeds its guest mask
  from the host mask at start-up. The same probe program
  shows the saved mask also carries QNX's always-masked top eight signals.
- **Real-time signals queue only with `SA_SIGINFO`.** With SIGRTMIN+3
  blocked and three `sigqueue`s, a zero-timeout `sigtimedwait` loop took 1
  with `SIG_DFL` and flags 0, 3 with `SIG_DFL` + `SA_SIGINFO`, 0 with
  `SIG_IGN` + `SA_SIGINFO`. Linux always queues them, so Linuxemu now gives
  real-time signals at the default action `SA_SIGINFO` (at start-up, in
  `rt_sigaction` and for the diagnostics handlers).
- **`sigwaitinfo` with `SIG_DFL`:** a thread blocking all signals in
  `sigwaitinfo({SIGUSR1, SIGRTMAX+8})` receives a process-directed SIGUSR1
  whose action is the default (the Task 1 probe had a handler installed),
  and `pthread_kill(thread, SIGRTMAX + 8)` (one of the top eight,
  always masked) wakes it with 64. signalfd's collector uses that signal
  to re-arm.

## 2026-11-20 — Phase 3 part 2: QNX drops pending signals at execve

- **Probe (native, Passport):** a process blocks SIGUSR1, forks; the child
  makes SIGUSR1 pending (`sigpending` shows it), then `execl`s itself. In
  the new image `sigpending` no longer holds SIGUSR1, whether it was sent
  with `kill(getpid())` or `pthread_kill(pthread_self())`. Linux keeps the
  pending set across `execve` (the calling thread's and the process's).
- **Consequence:** the design's plan for `exec` (re-raise the process queue
  just before the host `execve`, so the signals wait in the QNX kernel)
  cannot work, and every guest signal pending at `execve` was lost. Seen
  through Linuxemu too: `signalfd-semantics` check 32 got exit 1 (no
  handler run in the new image) on the Task 4 core.
- **Fix (Task 5):** the exec carries the Linux numbers of the signals
  pending for the caller (host `sigpending`, the thread's queue, the
  process queue) in `LINUXEMU_GUEST_PENDING`; the new image raises each
  again at start-up under the inherited blocked set, before the frame
  probe, which no longer unblocks anything Linuxemu started with. One
  instance each: sender details, `sigqueue` values and real-time counts
  are lost.
- **Task 6:** the carry keeps two sets. Signals pending for the process
  (host `sigpending`, which on QNX doesn't separate thread from process,
  and the process queue) are raised again with `kill(getpid())`, so the
  new image's signalfd collector can take them; signals only in the
  calling thread's queue are raised at the main thread.
- **Unguarded window:** a signal the collector takes after the exec's
  snapshot of pending signals and before the host `execve` is not in the
  carry, and the QNX kernel drops it at `execve`, so it is lost. Nothing
  guards that window.

## 2026-11-20 — QNX native inotify (Phase 3 part 3 probes)

Passport (QNX 8.0.0), ordinary user (uid 101431000). Probes:
`native-probes/phase3c-probes.c` (run twice: plain, and with `more`) and
`native-probes/phase3c-facts.c`, built warning-free by
`scripts/build-native-probes.sh` (`-Wall -Wextra -O2`; `phase3c-facts` links
`-lsocket`). The fixture directories were under the dev tree's `build/` (qnx6).
All three runs ended `probe_done=1` with no `probe_guard`. Every value below
is **measured** in this run unless marked otherwise; they agree with the
earlier design run.

Key evidence lines:

```
native_inotify_symbols=init:yes add:yes rm:yes
fsnotify_open=rdonly:3/0 rdwr:-1/13
fsevents_open=rdonly:3/0 rdwr:-1/13
ionotify_dir=ret:0xffffffff errno:22
rename_cookie_qnx6=paired from=8799 to=8799
subdir_qnx6=none
nonblock_read=setfl:0 ret:0 errno:0
poll_ready=ret:1 revents:0x1 latency_us:259339
small_read=ret:-1 errno:22
readd_same_wd=yes first:1 second:1
oneshot=wd1 [wd1 CREATE c0 l16 'o1'] [wd1 IGNORED c0 l0 '']
overflow=events:2069 q_overflow:yes
scan_cost_us=4178000 entries:1003
delivery_latency_us=n:40 min:7000 median:253000 p90:255000 max:255000
blocking_read_empty=ret:-1 errno:4 waited_ms:2001
blocking_read_wake=ret:32 waited_ms:581
fork_shared_queue=child_read:32 parent_after:none
# add_watch(/dev/shmem)=-1 errno=25   (/tmp, /pps, /proc likewise; qnx6, /base/bin, sdcard, / succeed)
err_enoent=ret:-1 errno:2
err_zero_mask=ret:1 errno:0
err_bad_wd=ret:-1 errno:22
onlydir_on_file=ret:-1 errno:20
fionread=ret:0 errno:0 value:32
dont_follow=ret:2 errno:0 [wd1 IGNORED c0 l0 '']
max_instances=300+ errno=0
max_watches=2000+ errno=0
identity=a:1/871157257 mode:0220664 rdev:1 b:1/871157257 dev_fsnotify:1/871157257 rdev:1 same_ab:yes same_dev:yes
# /dev/null identity=1/2 same_as_inotify:no
exec_survives=events
# rights sendmsg=1 errno=3
# rights child recvmsg=-1 errno=13 flags=0x0 controllen=16
rights_delivers=no_events
# fionread_full=65536
overflow_record=wd:-1 mask:0x4000 cookie:0 len:0 position:1
# overflow total_records=2049 overflow_records=1
```

- **Access and records.** libc exports `inotify_init`, `inotify_add_watch`
  and `inotify_rm_watch`; there is no `inotify_init1`. `/dev/fsnotify` and
  `/dev/fsevents` open read-only for us, `O_RDWR` gives `EACCES` (13).
  `struct inotify_event` is Linux's: 16-byte header, same mask bits, names
  NUL-padded to a multiple of 16 (`len` 16 for short names). Watch
  descriptors count from 1 per instance; re-adding a path returns the same
  wd. On qnx6 the events match Linux: `CREATE`, `OPEN`, `ACCESS`, `MODIFY`,
  `ATTRIB`, `CLOSE_WRITE`, `CLOSE_NOWRITE`, `DELETE`, `ISDIR`,
  `MOVED_FROM`/`MOVED_TO` with a shared cookie, `DELETE_SELF` then
  `IGNORED`, `IGNORED` after `inotify_rm_watch`. Subdirectory changes are
  not reported to the parent watch; changes by a forked child are.
  `IN_ONESHOT`, `IN_MASK_ADD`, `IN_ONLYDIR` and `IN_DONT_FOLLOW` work
  natively; the queue overflows (`IN_Q_OVERFLOW`) at 2,048 events. Blocking
  read with nothing queued is ended by a signal (`EINTR`) and woken by an
  event; a too-small buffer gives `EINVAL`; `FIONREAD` reports queued bytes;
  a forked child shares the queue. At least 300 instances and 2,000
  watches (the probe's caps; no limit was hit).
- **Five differences from Linux.** (1) A nonblocking read of an empty queue
  returns 0, not `EAGAIN`. (2) A zero mask is accepted (wd returned), Linux
  gives `EINVAL`. (3) Watches on `/dev/shmem`, `/pps` and `/proc` fail with
  `ENOTTY` (25); the probe's `/tmp` on the device also failed that way
  (it is not the guest's `/tmp`, which is the rootfs's qnx6 `tmp`). (4)
  Delivery is batched: median 253 ms, p90/max 255 ms over 40 trials, with a
  few at 7-39 ms when a batch boundary was near. (5) Background system
  processes open files and directories after changes, so `OPEN`/
  `CLOSE_NOWRITE`/`ACCESS` watches see extra, genuine events.
- **Device nodes can hang the call.** Measured 2026-11-20 during Task 2 with
  a scratch probe (5 s `alarm`): native `inotify_add_watch` on `/dev/null`,
  `/dev/ttyp0` and `/dev` returned `ENOTTY`, `/dev/tty` `ENXIO`, but on
  `/dev/ttyp1` and `/dev/ttyp5` it blocked (REPLY-blocked on the fsnotify
  server) until the signal arrived. Linuxemu therefore refuses character
  and block devices with `EOPNOTSUPP` before the native call.
- **Identity, exec, dup, `SCM_RIGHTS`.** `fstat` of two inotify fds returns
  the same `st_dev`/`st_ino` as `stat("/dev/fsnotify")` (1/871157257, mode
  `020664`); `/dev/null` differs, so an inotify fd is recognised by that
  comparison but two instances cannot be told apart. The native fd survives
  `execve` and keeps delivering events. `SCM_RIGHTS` fails natively:
  `sendmsg` returns 1 but the receiver's `recvmsg` fails with `EACCES` (13)
  and the data is lost. `dup`/`F_DUPFD`/`dup2` working natively was reported
  by the design run; these probes do not exercise them (not re-measured
  here; the fork result above shows the open-file description is shared).
- **Overflow record.** `wd` -1, mask `IN_Q_OVERFLOW` (0x4000), cookie 0, len
  0, as Linux. It is queued **first** (position 1 of 2,049 records), ahead
  of the 2,048 queued events; Linux appends it at the end. `FIONREAD` on a
  full queue reported 65,536.
- **Cost of scanning.** `readdir` + `lstat` over a 1,003-entry directory
  took 4,178,000 us (4.2 s), and directory `st_mtime` has whole-second
  resolution (1795182207 -> 1795182208), so a stat-scanning fallback is not
  viable.
- **`ionotify` returning `EINVAL`.** `ionotify()` on a directory fd returns
  -1 with `EINVAL` (22): there is no other native change-notification path.

## 2026-11-20 — Phase 3 part 3 gate: inotify-tools and the Python start-up crash

Passport (QNX 8.0.0), ordinary user, core `13eac1ec…` (`phase3c-inotify` at
`bd17b41`).

- **inotify-tools works on the native backend.** In the corpus run,
  `inotifywait` saw the created file (basic), all four files from four
  concurrent `touch` jobs (concurrent) and exited on `SIGTERM` (shutdown),
  with no unsupported call in its diagnostics report. The corpus waits of
  2 s comfortably cover QNX's ~250 ms batching.
- **The python3 start-up SIGSEGV is still intermittent.** The first full
  regression failed only `run-python-smoke`: python3 died before its first
  check (`fatal signal=0x0000000b pc=0x11210964 address=0x79616dcc`). Then:

  | Runs | Core | Result |
  | --- | --- | --- |
  | `run-python-smoke` alone, fresh install each, 5 times | part 3 | 1 failed (`pc=0x60067dc4 address=0x00000000`), 4 passed |
  | `run-python-smoke` alone, fresh install each, 5 times | pre-phase (`9a0f5844…`) | 5 passed |
  | the smoke's Python workload on one install, alternating cores, 15 rounds | each | 15 of 15 passed per core |
  | second full regression | part 3 | passed (`linuxemu_full_regression_failures=0`) |

  The isolated failure's `0x60067dc4`/address 0 signature is the one
  Phase 2 recorded on 2026-11-18, which also failed on that phase's
  pre-phase core; the regression failure's signature differs (nonzero
  fault address). Treating both as one crash is an inference from the
  matching conditions, not shown. Every
  failure so far was the first Python run after a fresh install; none was
  seen on a kept install. The sample doesn't separate the two cores (2 of
  7 against 0 of 5), Python's start-up has no inotify use, and the part 3
  code on its path is one `stat` of `/dev/fsnotify` at start-up. The
  cause is not isolated yet.
