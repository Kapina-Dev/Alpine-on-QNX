# BB10 native probe results — 2026-09-22

## Environment

- Device connection: `ssh bb10` (`blackberry@192.168.0.4`, port 2022).
- Device kernel string: `QNX BLACKBERRY-5F57 8.0.0 2018/02/21-10:54:19EST`.
- Ordinary credentials: UID/EUID 101431000, GID/EGID 10143.
- V2 root route: ordinary SSH, then `ssh -i "$HOME/.ssh/id_rsa_bb10" root@192.168.0.4`.
- The nested V2 root shell has UID/GID 0, starts in `/root`, and uses the native PATH `/usr/bin:/bin:/usr/sbin:/sbin`.
- Ordinary SSH can create files under `/tmp` and `/accounts/1000/shared/downloads`.
- V2 root could not create files in those locations during the same test.
- V2 root cannot execute newly built or BerryCore binaries. Copying the native probes to `/root/linuxemu-probes` and making them root-owned still produced `Operation not permitted`.

## Recovered SDK

- Laptop archive: `root-analysis/gcc-sdk.zip`.
- SHA-256: `ffd33f3af6187673b40776e0870d317f97e450c491185e6bc5644513905e27e7`.
- Device archive: `/accounts/1000/shared/misc/linuxemu-dev/gcc-sdk.zip`.
- Device extraction: `/accounts/1000/shared/misc/linuxemu-dev/sdk`.
- Compiler: GCC 4.6.3, target `arm-unknown-nto-qnx8.0.0eabi`.
- Sysroot: `sdk/target_10_3_1_995/qnx6`.
- The SDK host tools require `LD_LIBRARY_PATH=$QNX_HOST/lib`.
- The compiler does not accept `-pthread`, and the sysroot has no separate `libpthread`; pthread calls link through the native C library.

The reproducible device build is:

```sh
cd /accounts/1000/shared/misc/linuxemu-dev
sh scripts/build-native-probes.sh
```

## Passing probes

### Hello and basic baseline

```text
linuxemu native hello
hello_exit=0
uid=101431000 euid=101431000 gid=10143 egid=10143 page=4096
ucontext=368 mcontext=344 cpu=68 pc_offset=84 spsr_offset=88 main_tls=1086de70
worker=0 tls=10908b40 stable=1 error=0
worker=1 tls=10933e20 stable=1 error=0
baseline=PASS signal_count=1
baseline_exit=0
```

This verifies basic `SA_SIGINFO` delivery, stable native `__tls()` values, pthread keys, per-thread `errno`, and sequential thread create/join in the ordinary context.

### Simultaneous threads and directed signals

Four simultaneous threads each retained a distinct TLS address, pthread-specific value, and `errno` across 20,000 scheduling yields. Each received exactly one directed `SIGUSR1`.

```text
worker=0 tls=10108a40 stable=1 signals=1 error=0
worker=1 tls=101a59f0 stable=1 signals=1 error=0
worker=2 tls=101c7f00 stable=1 signals=1 error=0
worker=3 tls=1013cd00 stable=1 signals=1 error=0
thread_signal_stress=PASS threads=4 iterations=20000 unexpected_signals=0
thread_signal_stress_exit=0
```

### ARM undefined-instruction recovery

```text
mode=arm trap_count=1 si_code=1 fault=1100f774 saved_pc=1100f774 resume=1100f778 pc_delta=0 spsr=0x60000010 thumb_state=0
instruction_trap=PASS
instruction_trap_arm_exit=0
```

QNX saved PC at the exact four-byte ARM fault. Rewriting `gpr[15]` to the explicit assembler symbol immediately after the instruction resumed successfully.

### Thumb undefined-instruction recovery

```text
mode=thumb trap_count=1 si_code=1 fault=10894746 saved_pc=10894746 resume=10894748 pc_delta=0 spsr=0x60000030 thumb_state=1
instruction_trap=PASS
instruction_trap_thumb_exit=0
```

QNX saved PC at the exact two-byte Thumb fault. SPSR bit 5 recorded Thumb state, and rewriting `gpr[15]` to the following halfword resumed successfully.

An initial ARM trap implementation used GCC computed-label addresses. GCC coalesced the fault and resume labels to the same basic-block address, causing a second trap and safety exit 120. Explicit assembler symbols fixed the probe; this was a probe construction problem, not a QNX context-return failure.

## Current unresolved probe

`native-probes/executable-memory.c` tests writable mappings, generated ARM/Thumb instructions, instruction-cache synchronization, and executable protection transitions.

The current ARM writable-to-executable run produced:

```text
mode=arm stage=mapped mapping=10786000 initial_protection=0x300
Process 1181900950 (executable-memory-arm) terminated SIGSEGV code=2 fltno=11
executable_memory_arm_exit=139
```

The staged output stops before `stage=first_call`. The fault therefore occurs after the mapping and before the generated function call, within one of these operations:

1. writing the two ARM instructions;
2. GCC's `__builtin___clear_cache` helper;
3. `mprotect(PROT_READ | PROT_EXEC)`.

The writes are ordinary stores into a successful read/write mapping. Disassembly shows `__builtin___clear_cache` calling the target's cache-control helper. The next session should instrument around cache synchronization and `mprotect` separately, and inspect the SDK's QNX cache APIs before testing generated code again.

Direct-RWX comparison binaries were built but deliberately not run after the user asked to stop:

- `build/native-probes/executable-memory-arm-rwx`
- `build/native-probes/executable-memory-thumb-rwx`

Both currently invoke the same cache-clear helper, so running them without further instrumentation may reproduce the same fault without distinguishing executable-memory policy.

## Repository changes in this session

- Added `native-probes/hello.c`.
- Added `native-probes/thread-signal-stress.c`.
- Added `native-probes/instruction-trap.c`.
- Added `native-probes/executable-memory.c`.
- Added `scripts/build-native-probes.sh`.
- Updated `root-analysis/bb10-native-baseline.c` only to replace an old-GCC initializer warning with an explicit `memset`.

Device sources and build outputs are under `/accounts/1000/shared/misc/linuxemu-dev`. The verified SDK is already extracted there; it does not need to be transferred again.
