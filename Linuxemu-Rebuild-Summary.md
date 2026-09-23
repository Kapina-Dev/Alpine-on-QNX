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

Translate guest buffers through defined layouts and validate access. Unsupported functionality must fail predictably; do not use success stubs for locking or other operations whose semantics matter. Keep any deliberate approximation documented.

Gate: positive, failure-path and concurrent tests pass for each advertised feature.

### 5. Replace the threading workaround

Give each guest thread explicit host and guest state. Establish safe entry/exit trampolines and a retirement mechanism that runs on memory which remains valid. Implement real synchronization semantics rather than sleep-based futex stubs.

Gate: thousands of create/join and detach cycles, contended mutexes, condition variables, timeouts and signal interruptions complete correctly, without memory growing with each retired thread. Exercise fork from a multithreaded guest separately.

### 6. Restore applications progressively

Suggested regression order:
1. Minimal guest programs, BusyBox commands and shell scripts.
2. File manipulation, pipes, subprocesses and terminal behavior.
3. nano interactive editing and saving.
4. wget HTTP; then separately verified HTTPS/certificate handling.
5. apk index refresh, install/remove and trigger scripts using a verified repository snapshot.
6. Python scripts, imports, networking, subprocesses and threading.
7. Git clone/fetch and HTTPS, beyond git --version.

Select and pin an Alpine ARM release/rootfs only after checking its current availability and requirements. Preserve archive hashes and package versions for reproducible tests. Do not assume the historical Alpine 3.19 environment remains obtainable unchanged.

Gate: a clean installation can reproduce the supported application suite without undocumented manual fixes.

### 7. Package and define completion

A practical completed first version means:
- Documented supported device/OS/guest combinations.
- A reproducible build and installation procedure.
- A tested compatibility matrix, including explicit unsupported features.
- No known thread-exit leak or success stubs masking essential behavior.
- Predictable guest crashes/errors that do not alter global phone state.
- Automated regression tests and useful optional tracing.
- Source, release artifacts and test results backed up off-device.

It does not mean every Linux program works. Kernel modules, full Linux service management, graphics integration and arbitrary device ioctls need separate scope. X11 through Android XSDL was a historical optional direction, not a completed milestone; defer it until CLI, networking and threads are reliable.

## Immediate next actions

1. Confirm the outcome of the planned autoloader run and compare first-root versus next-boot filesystem state.
2. Once Downloads is writable, build the native baseline on the phone using the recovered SDK.
3. Preserve build output and run results; investigate ordinary-user execution before elevated execution.
4. Extend the native contract probes before rebuilding the Linux loader.
5. Begin the new repository and regression corpus; retain this document as the reconstruction checklist.

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
