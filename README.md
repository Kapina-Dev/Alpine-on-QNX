# Linuxemu

Linuxemu is an ARM32 Linux ABI translation layer for BlackBerry 10/QNX. Linux ARM instructions execute directly on the phone CPU. Linux operating-system interfaces are intercepted and translated to QNX.

The current rebuild has a verified native contract plus static and initial dynamic ARM execution paths. Alpine 3.24.2's musl interpreter can relocate and run the pinned BusyBox `true` and `echo` commands.

## Current verified behavior

- Native QNX signals and `ucontext_t` register edits in ARM and Thumb modes.
- Concurrent pthread TLS and directed signal delivery.
- Writable-to-executable memory transitions with QNX instruction-cache synchronization.
- Trap-emulated per-thread guest TLS reads in ARM and Thumb modes.
- Static ARM ELF validation, segment mapping, tracked ARM `svc #0` patching, and Linux process-entry stack construction.
- Linux ARM `write`, `exit`, `exit_group`, `mmap2`, `mprotect`, `set_tid_address`, process/credential queries, and `-ENOSYS` for unsupported calls.
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

Fetch the pinned Alpine 3.24.2 armhf fixture on the laptop:

```sh
sh scripts/fetch-alpine-test-rootfs.sh
```

Copy `bin/busybox` and `lib/ld-musl-armhf.so.1` from that rootfs into a `test-rootfs` directory in the device workspace, then run:

```sh
sh scripts/run-dynamic-smoke.sh
```

Detailed device evidence is recorded in `root-analysis/native-probe-results-20260922.md`.

## Current limits

- Dynamic support is currently limited to the pinned musl/BusyBox smoke path; shared-library file mapping and general rootfs path translation remain incomplete.
- Guest threading and signal semantics are not implemented; the current kuser and TLS state covers one guest thread.
- ARM `svc #0` patching only; Thumb guest instruction scanning is not implemented.
- Initial `argc`/`argv`/`envp` and core auxiliary vectors.
- Only `write`, `exit`, and `exit_group` are translated.
- Executable-segment scanning currently targets the exact ARM `svc #0` word. A section-aware or decoded patch pass is required before accepting general binaries.
