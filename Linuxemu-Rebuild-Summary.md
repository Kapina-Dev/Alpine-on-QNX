# Linuxemu: progress and rebuild plan

Updated: 2026-09-23

## Goal and current state

Rebuild Linuxemu as a reliable Linux ARM32 ABI translation layer for Alpine/musl applications on BlackBerry 10. Linux instructions execute on the ARM CPU; the translator supplies the Linux process environment and translates operating-system interfaces to QNX. This is not a Linux kernel, container, or guarantee that all Linux software will run.

The earlier source and executable are lost on both devices. The surviving project notes describe substantial working progress, but cannot replace the implementation or reproducible test results. The new native probe exists as source only; it has not been compiled or run.

Historical work targeted the Passport. Current root investigation is on a Q20/Classic. Results must identify the device instead of assuming identical behavior.

## Earlier progress worth preserving

The surviving notes dated April 15–17, 2026, and the user's experience report:

| Area | Previously achieved | Evidence limit |
| --- | --- | --- |
| Loading | Alpine 3.19 armhf ELF programs and musl dynamic linker | Historical report; loader source lost |
| Basic applications | BusyBox shell/utilities, nano, HTTP wget | Restore as regression tests |
| Packages | apk update/add, package extraction and install triggers | Repository/package availability must be checked again |
| Python | Installed Python and ran a simple program | Not evidence that all Python modules worked |
| Git | Installed Git and ran git --version | Does not establish clone, HTTPS, or subprocess coverage |
| Processes | fork/exec/wait and rootfs environment propagation used by applications | Exact supported semantics unknown |
| Threads | Reported 10 threads × 100 mutex increments = 1000, sleep/lock and joins | Used a serious thread-exit memory-leak workaround |

The old design patched ARM Linux svc instructions to undefined instructions, caught SIGILL, and dispatched Linux syscalls through QNX. Only ARM syscall patching was documented; Thumb syscall support remained absent.

Useful lessons from the notes:
- Translate errno, socket constants/flags, ioctl structures and terminal settings explicitly.
- The terminal fix used separate Linux and QNX termios layouts; directly passing a guest buffer had corrupted memory.
- Preserve rootfs configuration across exec, including callers that filter environment variables.
- Keep host library search paths out of the guest environment.
- Return guest-visible paths rather than leaking host rootfs prefixes.
- Keep tracing optional, but retain useful unsupported-syscall diagnostics.
- A historical select-based workaround addressed nonblocking connection readiness; reproduce the problem before retaining the workaround.

The old notes call the syscall set “full,” but also record missing calls and stubs. Treat application success as application-level evidence, not ABI completeness. Likewise, old explanations about Alpine repository errors, package availability, and TLS behavior remain historical claims to recheck.

## The central unresolved problem: host and guest TLS

The old translator patched reads of TPIDRURO to TPIDRURW. Notes describe restoring guest TLS at dispatch entry and a crash when musl retired a thread's stack/TLS mapping while QNX code still referenced that memory.

The workaround skipped munmap when it overlapped the current guest TLS area and returned success. Reported leakage was approximately 2 MB per exiting thread. It allowed useful tests to pass but is not acceptable as the final lifetime model.

The rebuild needs independently verified rules for:
1. Where native QNX TLS lives and how libc and kernel-call stubs access it.
2. Which TLS registers remain stable across scheduling, signal entry/return and native calls.
3. How to enter host code with valid host TLS, then restore guest TLS before returning to guest instructions.
4. How signals arriving during that transition find valid per-thread state.
5. Who owns native stacks, guest stacks, guest TLS and signal-dispatch state.
6. How thread exit clears guest TID state, wakes waiters and frees memory only after the last user is finished.

Do not assume a global TLS variable can represent concurrent threads. Do not promise successful unmapping while permanently retaining the allocation.

## What the current investigation established

### Root and execution policy

The current phone reports BB10 10.3.3.3216 and a QNX kernel release string of 8.0.0. That string does not establish compatibility with modern QNX SDP 8.

Credential checks established:
- Ordinary SSH shell: real/effective/saved UID 101031000, GID 10103.
- __root: real/effective/saved UID and GID 0.
- u_root: real/effective/saved UID 0, GID 10103.

Native /bin/ls runs in these contexts. BerryCore ls/id run in the ordinary context but were denied in elevated contexts. Read-only pathtrust queries reported native tools trusted and those BerryCore tools untrusted. Root does not automatically make every executable usable.

The common wrapper attempts ability changes without checking return values. SDK definitions decode observed flags as NONROOT | ALLOW | INHERIT_YES, with SUBRANGE in the second form. Attempted requests do not prove actual granted abilities.

Root can help inspect otherwise restricted process state and diagnose permission failures. It does not supply Linux syscall semantics or solve TLS ownership. Prefer ordinary-user operation where verified sufficient; isolate any truly necessary privileged setup.

### SDK recovered

The device's cached gcc.zip was a 134-byte Git LFS pointer, not an installable archive. The actual archive is now on the laptop:

- Local: root-analysis/gcc-sdk.zip
- Source: https://media.githubusercontent.com/media/sw7ft/BerryCore/main/berrycore/packages/gcc.zip
- Size: 108300360 bytes
- SHA-256: FFD33F3AF6187673B40776E0870D317F97E450C491185E6BC5644513905E27E7

The hash matches the pointer. The archive includes an ARM-native GCC 4.6.3 toolchain and target_10_3_1_995/qnx6 headers. It is not a Windows compiler. Current BerryCore tools belong under /accounts/1000/shared/misc/berrycore; clitools is a compatibility symlink.

Header inspection established:
- ARM signal context contains gpr[16] and spsr, with PC=15, SP=13, LR=14.
- ucontext_t exposes this through uc_mcontext.cpu.
- signal.h marks SA_ONSTACK, SA_RESTART and sigaltstack as unsupported.
- sys/storage.h declares __tls(), but does not establish the hardware TLS-register contract.

These are BB10_3_1 SDK definitions, not verified runtime behavior on BB10 10.3.3. Do not invent missing constants to force unsupported signal features.

### Device state currently blocks building

Creating build directories under both misc and downloads returned “Read-only file system.” Creating a directory under /tmp, which links to /dev/shmem, also failed, with “No such file or directory.” The latter does not establish that all temporary file writes are impossible.

Earlier logs showed fs-qnx6 corruption followed by write and Settings sandbox failures. This supports a connection between storage failure and apps not opening. It does not establish which root operation caused the corruption.

install.sh repeats during boot. The hypothesis that the repeated sequence breaks the filesystem is unproven. Leftover getroot PPS entries have not been established as the cause of launcher failures. Do not carry an assumed PPS cleanup fix into Linuxemu.

The user plans to rerun the autoloader; completion and results have not been verified. Back up needed data first. Compare app launches, a small write test and filesystem logs after first root completion and again after one reboot.

## Rebuild stages and acceptance gates

### 1. Stable device and reproducible toolchain

- Establish writable storage and usable apps before and after reboot.
- Install the verified SDK into an isolated writable directory.
- Record compiler version, target, build flags, dependencies and source hashes.
- Create a version-controlled repository with off-device backups from the first commit.
- Keep logs for each device, boot and execution context.

Gate: a native hello-world program builds and runs repeatedly; storage stays writable during the tests.

### 2. Verify the native signal/TLS/thread contract

Start with root-analysis/bb10-native-baseline.c. It currently covers credentials, structure offsets, basic SA_SIGINFO delivery, native TLS pointer stability, pthread key/errno behavior and sequential thread creation/join. It has not been built. It does not test hardware-register switching, simultaneous thread isolation or fault recovery.

Add isolated probes for:
- Simultaneous threads, distinct per-thread state, directed signals and scheduling stress.
- ARM and Thumb undefined-instruction traps: observed PC, saved state, register edits and return to an explicit resume label.
- Signal masks, interrupted calls and nested delivery; investigate alternatives to unavailable native alternate stacks.
- Memory permissions, writable-to-executable transitions and instruction-cache synchronization.
- Hardware TLS register reads, then controlled switching with no host calls made under invalid TLS.
- Repeated thread teardown with bounded memory use.
- fork/exec and credential/ability inheritance.

Each probe needs a process timeout, exit status and preserved output. A process alarm is useful but insufficient for tests that alter signal handling; use an external timeout as well.

Gate: an evidence document states exactly which transitions work on each tested device. No claims of a completed contract from header inspection alone.

### 3. Build a small, explicit execution core

Separate modules for ELF loading, instruction interception, guest memory, ABI conversion, syscall dispatch, process/thread state, signals and diagnostics.

- Validate ELF class, machine, byte order, segments and bounds.
- Construct the guest stack, argv/envp and auxiliary vector correctly.
- Support the musl interpreter and shared-library mappings required by the chosen guest.
- Define address-space ownership and collision handling.
- Handle ARM and Thumb execution explicitly.
- Avoid blind byte-pattern rewriting that can alter embedded data; track patched executable locations and originals.
- Cover libraries mapped after startup and changes to executable mappings.
- Start with tiny write/exit guests before adding application complexity.

Gate: reproducible minimal static and dynamic guest programs pass, with correct exit codes and no silent memory corruption.

### 4. Implement Linux semantics, not just similarly named calls

Build an explicit compatibility matrix. For every supported operation record its Linux ABI, QNX implementation, structure conversions, error mapping and tests.

Priority areas:
- File descriptors, access flags, stat layouts, offsets, directory entries, links and path resolution.
- mmap/mprotect/munmap, brk, alignment and allocation ownership.
- Terminal ioctls, descriptor flags and polling.
- Socket layouts/options, DNS, blocking/nonblocking behavior and connection errors.
- Clocks, timeout conversion, absolute deadlines and time32/time64 variants.
- fork/exec/wait, pipes, descriptor inheritance, environment and exit status.
- Signals, masks, interruption, restart semantics and guest signal frames.
- clone/thread lifecycle, futex wait/wake races, TLS setup and clear_child_tid behavior.

Deferred polling correctness item recorded after the 2026-09-22 Phase 4 implementation:
- `ppoll` and `pselect6` currently convert the Linux signal mask, install it with `sigprocmask`, perform the QNX wait, and restore the old mask. QNX 10.3 provides no native `ppoll` or `pselect` entry point, so mask installation and the wait are not atomic and a signal can arrive in between.
- Keep the working implementation for basic terminal and readiness workloads, but do not claim signal-race-equivalent Linux semantics.
- Investigate QNX `_select_event`, `timer_timeout`, or another kernel-assisted wait that can combine signal notification with descriptor readiness. Do not replace the current path with a helper-thread design unless cancellation, descriptor reuse, and process-fork behavior are defined.
- Add a stress test that repeatedly delivers a signal in the mask-transition window and proves there is no lost wakeup before this item is closed.

Phase 5 networking status recorded on 2026-09-23:
- Direct ARM socket syscalls 281 through 297 and `accept4`, plus legacy `socketcall`, are translated for the currently tested operations.
- UNIX, IPv4, and IPv6 address layouts; socket creation flags; message flags; common `SOL_SOCKET`, IPv4, and TCP options; and QNX network errno values are converted explicitly.
- Synthetic guests pass socket-pair, UDP, send/receive, message-header, option, descriptor-flag, nonblocking-connect, readiness, peer-address, and `SO_ERROR` paths.
- BusyBox `wget` passes against native loopback HTTP servers through numeric IPv4 and `localhost` IPv6. BusyBox `nslookup` resolves through the configurable LAN resolver, and a refused connection reports Linux `ECONNREFUSED`.
- Ancillary/control messages and socket options outside the explicit compatibility table remain unsupported.

Phase 6 HTTPS and package status recorded on 2026-09-23:
- Corrected the ARM `stat64` alignment gap before `st_size`; the earlier layout corrupted sizes and final inode values, causing musl to mistake `libcrypto` for an already loaded library.
- Added `readv`, a QNX-compatible gathered `writev`, `getrandom` backed by the device entropy source, `fstatfs64`, `umask`, and unprivileged advisory locking.
- Added contained legacy and at-family directory, unlink, rename, hard-link, symlink, chmod, access, and timestamp operations required by package extraction and database updates.
- Guest `execve` recognizes shebang files, resolves the interpreter inside the rootfs, and constructs interpreter/optional-argument/script argv entries. Optional syscall tracing survives the host-side exec trampoline without entering the guest environment.
- BusyBox HTTPS downloads the pinned Alpine 3.24 ARM index through OpenSSL and rejects the same server certificate when given an empty trust store.
- `apk update` reads both pinned repositories. A clean nano transaction installs three packages, executes the BusyBox trigger and nano binary, removes all three packages, and restores the original world file with zero failures.
- Apk runs in explicit unprivileged `--no-chown` mode. QNX timestamp support currently preserves seconds, and `flock` uses QNX record locking rather than Linux open-file-description semantics.

Phase 7 thread and futex status recorded on 2026-09-23:
- Replaced the process-global guest TLS word with QNX pthread-specific runtime state and synthetic Linux TIDs. Thread clone preserves the guest register set, starts on the requested guest stack, and keeps QNX thread retirement on the native stack.
- Implemented `gettid`, real `set_tid_address`, clone parent/child TID stores, and clear-and-wake behavior for `clear_child_tid`.
- Implemented futex wait/wake, requeue/compare-requeue, bitset waits/wakes, and timeouts with the value check and waiter insertion protected by one registry lock.
- A four-thread synthetic mutex test completed 2,000 protected updates, and 2,000 sequential clone/exit cycles completed without lost wakeups, TLS cross-talk, or creation exhaustion.
- A pinned Alpine musl fixture passes four pthreads, mutex contention, condition wait/broadcast, joins, and a timed condition wait. Musl condition broadcast exposed and now covers `FUTEX_REQUEUE_PRIVATE`.
- Added the process-clone form used by musl `posix_spawn`; the Alpine guest compiler successfully built the pthread fixture inside Linuxemu.
- General asynchronous guest signal frames, cancellation signals, priority-inheritance futexes, robust lists, and cross-process shared futexes remain outside the verified surface.

Phase 8 signal status recorded on 2026-09-23:
- Added Linux ARM classic and real-time signal frames with VFP state, register and mask restoration, emulator-owned return trampolines, and guest-provided restorer support.
- Signal masks, pending bits, alternate stacks, and interrupted futex state are per guest thread. Linux `tkill` and `tgkill` target synthetic Linux TIDs through the QNX pthread registry.
- Direct tests cover asynchronous standard and real-time handlers, handler-time mask changes, alternate-stack execution, `sigsuspend` interruption, and recovery from a synchronous guest `SIGILL` by editing the saved context.
- A blocked pipe read interrupted by a handler with `SA_RESTART` resumes at the original syscall and completes after data arrives; non-restartable waits still report Linux `EINTR`.
- Replaced futex condition waits with semaphore waits that signal handlers can interrupt safely. A real Alpine musl thread blocked in a condition wait now cancels and joins as `PTHREAD_CANCELED`.
- Queued duplicate real-time instances and an atomic QNX implementation of the `ppoll`/`pselect6` temporary-mask transition remain unresolved and explicitly unsupported.

Translate guest buffers through defined layouts and validate access. Unsupported functionality must fail predictably; do not use success stubs for locking or other operations whose semantics matter. Keep any deliberate approximation documented.

Gate: positive, failure-path and concurrent tests pass for each advertised feature.

## Remaining execution phases

The original application-and-packaging stages were revised after the Phase 7
thread work. Signals, loader/process stress, Python, Git, and release packaging
have different failure surfaces and now have separate acceptance gates.

### Phase 8. Signals and interruption

- Implement Linux `rt_sigaction`, per-thread masks, pending delivery, guest
  signal frames, and `rt_sigreturn` without exposing QNX context layouts.
- Cover synchronous faults separately from asynchronous signals and preserve
  Linux `EINTR` and restart behavior across blocking calls.
- Exercise the signal path used by pthread cancellation.
- Resolve the `ppoll`/`pselect6` mask-transition race with a QNX kernel-assisted
  primitive, or retain and precisely document the bounded incompatibility.

Gate: direct guest handlers and return, nested masks, timed waits, cancellation,
and signal interruption pass on the device. The polling mask transition must be
proven atomic or remain explicitly outside the supported compatibility surface.

### Phase 9. Loader and process/thread hardening

- Remove the intermittent exact-address mapping collision that currently makes
  some guest tool invocations require retries.
- Exercise `dlopen`, executable mappings created after startup, and failure
  cleanup without weakening exact Linux address checks.
- Stress fork, exec, and musl `posix_spawn` from a multithreaded guest and prove
  that copied thread/futex state is reset correctly in the child.
- Use root-only `/proc/<pid>` and `pidin` access for diagnostic evidence when
  needed. Ordinary-user execution remains the acceptance environment and must
  not depend on privileged process inspection.

Gate: repeated loader and process/thread stress runs complete without retries,
stale host state, leaked mappings, or guest-root filesystem damage.

### Phase 10. Python runtime

- Pin an Alpine Python version and package set, preserving archive hashes.
- Test imports, files, clocks, SSL, sockets, subprocesses, signals, and Python
  threads from a clean rootfs state.
- Reject unsupported operations with Linux errors instead of success stubs.

Gate: a reproducible Python application suite passes from a clean rootfs.

### Phase 11. Git workflows

- Cover local repository creation, status, add, commit, and object operations.
- Cover HTTPS clone and incremental fetch against pinned test content, including
  invalid-certificate, authentication, and unreachable-server failures.
- Stress subprocess behavior and transfers larger than the smoke fixtures.

Gate: local workflows, clean clone, and incremental fetch pass with correct
failure behavior.

### Phase 12. Package and define completion

- Document supported device, OS, guest, syscall, and application combinations.
- Provide reproducible build and installation procedures plus a single full
  regression entry point.
- Preserve pinned rootfs/archive hashes, release artifacts, test results, and
  an off-device backup.
- Require predictable guest failures that do not alter global phone state.

Gate: a clean installation reproduces the supported suite without manual repair
or retry loops.

Priority-inheritance futexes, robust mutexes, process-shared futexes, Thumb
execution, unusual socket control messages, kernel modules, full Linux service
management, graphics integration, and arbitrary device ioctls are demand-driven
compatibility items. Implement them for a selected workload or reject them
explicitly; they do not block the first supported release by default.

## Immediate next action

Execute Phase 8 against the completed thread/futex baseline, preserving the
full earlier regression suite as its non-regression gate.

## Evidence and local artifacts

Paths below are on the laptop unless explicitly identified as phone paths.

- Historical project notes: D:/chromedl/project_linuxemu.md
- Original root investigation: C:/Users/dimit/Desktop/root.md.txt
- Workspace: C:/Users/dimit/Documents/ChatGPT/Blackberry
- Current findings: root-analysis/root-contract-followup.md
- Earlier offline root analysis: root-analysis/findings.md
- Native probe source: root-analysis/bb10-native-baseline.c
- SDK archive: root-analysis/gcc-sdk.zip
- Collection scripts: Collect-BB10Root.ps1 and Collect-LinuxEmuContract.ps1
- Initial probe roadmap: LinuxEmu-Contract-Plan.md (its alternate-stack proposal is superseded by the SDK finding above)
- Collected root results: bb10-results-20260921-203225
- Collected contract survey: bb10-contract-20260921-192900
- Copied binaries/bytecode: bb10-root-artifacts-20260921-204208

Historical observations, current verified findings and proposed work are deliberately distinguished throughout this document.
