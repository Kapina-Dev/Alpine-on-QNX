# Linuxemu

Linuxemu is an ARM32 Linux ABI translation layer for BlackBerry 10/QNX. Linux ARM instructions execute directly on the phone CPU. Linux operating-system interfaces are intercepted and translated to QNX.

The current rebuild has a verified native contract plus static and dynamic ARM execution paths. Alpine 3.24.2's musl interpreter can relocate and run BusyBox commands against a contained guest root filesystem.

## Current verified behavior

- Native QNX signals and `ucontext_t` register edits in ARM and Thumb modes.
- Concurrent pthread TLS and directed signal delivery.
- Writable-to-executable memory transitions with QNX instruction-cache synchronization.
- Trap-emulated per-thread guest TLS reads in ARM and Thumb modes.
- Static ARM ELF validation, segment mapping, tracked ARM `svc #0` patching, and Linux process-entry stack construction.
- Linux ARM file descriptors and I/O: `open`, directory-relative `openat`, `close`, `read`, `write`, `writev`, `lseek`, `_llseek`, `dup`, `dup2`, and the required `fcntl64` subset.
- Linux ARM filesystem metadata and traversal: `stat64`, `lstat64`, `fstat64`, `fstatat64`, `readlink`, `readlinkat`, `getdents64`, and `getcwd`.
- Linux ARM memory calls: `brk`, `mmap2`, `mprotect`, and `munmap`, plus process/credential queries, guest TLS setup, exits, and `-ENOSYS` for unsupported calls.
- Rootfs path normalization and guest-aware symlink traversal. Absolute symlink targets remain inside the guest root, and `..` cannot escape above guest `/`.
- Linux ARM process calls for shell workloads: pipes, fork and fork-like `vfork`/`clone`, `execve`, `wait4`, process IDs/groups, working-directory changes, and exit-status propagation.
- Exec preserves guest argv, cwd, rootfs selection, ordinary environment variables, and descriptor inheritance. Host-only `LINUXEMU_ROOT` and `LD_LIBRARY_PATH` values are removed from the guest environment.
- The noninteractive SIGCHLD action/mask/suspend path needed by BusyBox background jobs. General guest signal delivery remains outside this phase.
- Linux ARM terminal calls with explicit termios, control-character, baud-rate, window-size, process-group, `FIONREAD`, and `FIONBIO` conversion. BusyBox `stty` can read and update an SSH pseudo-terminal.
- Linux ARM readiness calls: `poll`, `ppoll`, legacy and new `select`, and `pselect6`, including time32/time64 timeout conversion and Linux/QNX poll-event mapping.
- Linux ARM wall and monotonic clocks, resolution queries, `time`, `gettimeofday`, `nanosleep`, and `clock_nanosleep`, with explicit time32/time64 layouts. BusyBox `date` and `sleep` work against the pinned rootfs.
- Linux ARM socket calls for UNIX, IPv4, and IPv6 endpoints, including explicit address, type/descriptor flag, message flag, socket option, and network errno conversion. Both direct ARM socket syscalls and legacy `socketcall` are accepted.
- Blocking and nonblocking TCP connections, UDP datagrams, socket pairs, DNS queries, and BusyBox HTTP downloads. Numeric IPv4 and `localhost` over IPv6 are covered on the device.
- HTTPS through Alpine's OpenSSL-backed `ssl_client`, including device entropy through Linux `getrandom` and certificate-chain rejection. The pinned Alpine package index downloads and validates as gzip data.
- Alpine `apk` repository refresh, package extraction, BusyBox trigger scripts, installed dynamic applications, and package removal. A clean nano transaction restores the original world file.
- Contained rootfs mutation through legacy and directory-relative mkdir, unlink/rmdir, rename, link, symlink, chmod, access, and timestamp calls. Linux shebang execution restarts Linuxemu with the guest interpreter and argv layout.
- Thread-style `clone` backed by detached QNX pthreads, with separate host lifecycle state, synthetic Linux TIDs, per-thread guest TLS, parent/child TID stores, and `clear_child_tid` wakeup after returning to the native QNX stack.
- Futex wait, wake, requeue, compare-and-requeue, bitset selection, and relative/absolute timeout paths. Alpine musl pthread creation, joins, mutex contention, condition broadcast, and timed condition waits pass on the device.
- Linux ARM classic and real-time signal frames, handler return, per-thread masks, alternate signal stacks, synchronous guest `SIGILL`, `sigsuspend` interruption, thread-directed delivery, and musl pthread cancellation.
- The process-group query and signal subset needed for BusyBox interactive-shell startup on QNX, including a `getpgid(0)` fallback for QNX's nonfunctional libc stub.
- PIE and musl interpreter loading at separate biases, Linux kuser helper emulation, guest TLS setup, and explicit Linux-to-QNX memory-protection conversion.

TPIDRURW must not hold persistent guest TLS. This QNX build does not context-switch it per pthread; values bleed between threads and CPUs. Guest TLS reads must be trapped and emulated.

## Device build

The phone workspace is `/accounts/1000/shared/misc/linuxemu-dev`. With the recovered SDK extracted to its `sdk` directory:

```sh
cd /accounts/1000/shared/misc/linuxemu-dev
sh scripts/build-native-probes.sh
```

Run the non-destructive native contract suite:

```sh
sh scripts/run-native-probes.sh
```

Run the minimal Linux guest suite:

```sh
sh scripts/run-linuxemu-smoke.sh
```

Run malformed-ELF and occupied-address rejection tests:

```sh
sh scripts/run-loader-negative-tests.sh
```

Fetch the pinned Alpine 3.24.2 armhf fixture on the laptop:

```sh
sh scripts/fetch-alpine-test-rootfs.sh
```

Extract that rootfs as `test-rootfs` in the device workspace, then run:

```sh
sh scripts/run-dynamic-smoke.sh
```

Run the file, metadata, path-containment, and shell-redirection suite:

```sh
sh scripts/run-filesystem-smoke.sh
```

Run pipelines, child execution, waiting, cwd inheritance, and environment tests:

```sh
sh scripts/run-process-smoke.sh
```

Run clock, sleep, poll/select, terminal conversion, and interactive-shell tests. Allocate a pseudo-terminal so the terminal round trip runs:

```sh
ssh -tt bb10 'cd /accounts/1000/shared/misc/linuxemu-dev && sh scripts/run-terminal-time-smoke.sh'
```

Run direct socket coverage, nonblocking connection readiness, loopback HTTP,
DNS, and connection-error tests:

```sh
sh scripts/run-network-smoke.sh
```

The DNS test defaults to resolver `192.168.0.1` and name `example.com`.
Override them with `DNS_SERVER` and `DNS_NAME` when the device is on another
network.

Run HTTPS certificate, repository refresh, package install/trigger/execute/remove,
and cleanup tests:

```sh
sh scripts/run-package-smoke.sh
```

This suite temporarily writes the configured resolver into the guest rootfs and
restores the prior resolver and package world state on exit.

Run synthetic thread/futex races, 2,000 thread lifecycle cycles, and the pinned
Alpine musl pthread fixture:

```sh
sh scripts/run-thread-smoke.sh
```

The fixture source is `guest-tests/pthread-smoke.c`. To rebuild its ARM binary,
temporarily install Alpine `build-base` in the test rootfs and run
`sh scripts/build-pthread-fixture.sh`.

Run direct signal-frame, mask, alternate-stack, synchronous-fault, real-time,
thread-directed, and interruption checks:

```sh
sh scripts/run-signal-smoke.sh
```

Detailed device evidence is recorded in `root-analysis/native-probe-results-20260922.md`.

## Execution-core layout

- `elf_loader.c`: ELF and interpreter validation, executable-section selection, and load orchestration.
- `guest_memory.c`: guest address ownership, collision-safe mappings, `brk`, final permissions, and cache synchronization.
- `guest_path.c`: rootfs path normalization, contained symlink resolution, and guest working-directory state.
- `linux_abi.c`: explicit Linux errno, open-flag, status-flag, and `stat64` conversion.
- `guest_process.c`: contained ELF/shebang exec trampoline and guest-to-host process environment boundary.
- `guest_thread.c`: per-QNX-thread guest state, Linux clone/TID behavior, native-stack retirement, and fork reset.
- `linux_futex.c`: locked futex waiter registry, wake/requeue operations, and timeout conversion.
- `guest_signal.c`: signal-number conversion, per-thread masks, Linux ARM signal frames, alternate stacks, delivery, and return.
- `linux_time.c`: Linux time32/time64, clock-ID, resolution, and sleep conversion.
- `linux_poll.c`: poll-event, descriptor-set, timeout, and temporary signal-mask conversion.
- `linux_socket.c`: socket addresses, flags, options, message headers, errors, and direct/legacy socket syscall dispatch.
- `linux_termios.c`: Linux terminal flags, baud rates, control characters, window sizes, and related ioctl conversion.
- `arm_patch.c`: decoded ARM syscall and TPIDRURO instruction records.
- `trap.c`: SIGILL/SIGSEGV dispatch and Linux ARM kuser helpers.
- `linux_syscall.c`: the currently supported Linux syscall translations.
- `runtime.c`: process entry, initial guest stack, and auxiliary vectors.
- `linuxemu.c`: initialization and handoff only.

## Current limits

- Dynamic support is currently limited to the pinned Alpine musl/BusyBox path.
- `SA_RESTART` is implemented for the translated blocking read/write, wait, futex, ioctl, and socket calls. Linux real-time signals are mapped only while QNX real-time numbers are available, and pending instances of the same signal are currently coalesced rather than queued.
- ARM `svc #0` patching only; Thumb guest instruction scanning is not implemented.
- `vfork` and process-style `clone` currently use host `fork`; the `CLONE_VM | CLONE_VFORK | SIGCHLD` form used by musl `posix_spawn` is accepted without shared-address-space semantics.
- Futex priority-inheritance and robust-list operations are unsupported. Non-private futex calls only synchronize threads inside one Linuxemu host process, so process-shared futexes across `fork` are not supported.
- `ppoll` and `pselect6` install the requested host signal mask around the wait, but QNX 10.3 lacks native entry points that make the mask replacement and wait one atomic operation.
- Clock IDs beyond realtime, monotonic, process CPU time, and thread CPU time are rejected.
- Socket ancillary/control messages and unlisted socket options are rejected. The DNS smoke test requires a reachable resolver and is bounded by an external timeout.
- Package tests use apk's unprivileged `--no-chown` mode. Nanosecond timestamps are reduced to the seconds available through QNX `utime`, and Linux `flock` is approximated with QNX `fcntl` record locks.
- ARM patching is limited to 32-bit ARM instructions in validated `SHF_EXECINSTR` sections. Thumb instruction decoding remains unsupported.
- Section headers are currently required so the loader can avoid patching embedded data in executable segments.
