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

**Device:** Passport at 192.168.0.3, QNX 8.0.0 build 2018/02/21. Run over SSH
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
