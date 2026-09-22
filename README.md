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

Detailed device evidence is recorded in `root-analysis/native-probe-results-20260922.md`.

## Execution-core layout

- `elf_loader.c`: ELF and interpreter validation, executable-section selection, and load orchestration.
- `guest_memory.c`: guest address ownership, collision-safe mappings, `brk`, final permissions, and cache synchronization.
- `guest_path.c`: rootfs path normalization, contained symlink resolution, and guest working-directory state.
- `linux_abi.c`: explicit Linux errno, open-flag, status-flag, and `stat64` conversion.
- `arm_patch.c`: decoded ARM syscall and TPIDRURO instruction records.
- `trap.c`: SIGILL/SIGSEGV dispatch and Linux ARM kuser helpers.
- `linux_syscall.c`: the currently supported Linux syscall translations.
- `runtime.c`: process entry, guest stack, auxiliary vectors, and single-thread TLS state.
- `linuxemu.c`: initialization and handoff only.

## Current limits

- Dynamic support is currently limited to the pinned Alpine musl/BusyBox path.
- Guest threading and signal semantics are not implemented; the current kuser and TLS state covers one guest thread.
- ARM `svc #0` patching only; Thumb guest instruction scanning is not implemented.
- The guest working directory is fixed at `/`; `chdir` and broader directory mutation calls are not implemented yet.
- ARM patching is limited to 32-bit ARM instructions in validated `SHF_EXECINSTR` sections. Thumb instruction decoding remains unsupported.
- Section headers are currently required so the loader can avoid patching embedded data in executable segments.
