# Linuxemu

Linuxemu is an ARM32 Linux ABI translation layer for BlackBerry 10/QNX. Linux ARM instructions execute directly on the phone CPU. Linux operating-system interfaces are intercepted and translated to QNX.

The current rebuild has a verified native contract and a minimal static ARM execution core. It is not yet an Alpine or musl loader.

## Current verified behavior

- Native QNX signals and `ucontext_t` register edits in ARM and Thumb modes.
- Concurrent pthread TLS and directed signal delivery.
- Writable-to-executable memory transitions with QNX instruction-cache synchronization.
- Trap-emulated per-thread guest TLS reads in ARM and Thumb modes.
- Static ARM ELF validation, segment mapping, tracked ARM `svc #0` patching, and Linux process-entry stack construction.
- Linux ARM `write`, `exit`, `exit_group`, and `-ENOSYS` for unsupported calls.

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

Detailed device evidence is recorded in `root-analysis/native-probe-results-20260922.md`.

## Current limits

- Static ARM `ET_EXEC` guests only.
- ARM `svc #0` patching only; Thumb guest instruction scanning is not implemented.
- Initial `argc`/`argv`/`envp` and core auxiliary vectors; dynamic interpreter loading remains to be added.
- Only `write`, `exit`, and `exit_group` are translated.
- Executable-segment scanning currently targets the exact ARM `svc #0` word. A section-aware or decoded patch pass is required before accepting general binaries.
