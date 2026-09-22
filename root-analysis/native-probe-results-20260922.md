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

## Executable memory and instruction-cache synchronization

`native-probes/executable-memory.c` tests writable mappings, generated ARM/Thumb instructions, instruction-cache synchronization, and executable protection transitions.

The initial ARM writable-to-executable run produced:

```text
mode=arm stage=mapped mapping=10786000 initial_protection=0x300
Process 1181900950 (executable-memory-arm) terminated SIGSEGV code=2 fltno=11
executable_memory_arm_exit=139
```

The staged output stopped before `stage=first_call`. SDK header inspection established that BB10 exposes instruction synchronization as `msync(address, length, MS_INVALIDATE_ICACHE)`. GCC's `__builtin___clear_cache` generated that call, but the original probe invoked it while the page was still read/write. On this device, the mapping must have `PROT_EXEC` before instruction-cache invalidation.

The corrected sequence is:

1. map or protect the page read/write;
2. write instructions;
3. change the page to read/execute;
4. call `msync(..., MS_INVALIDATE_ICACHE)` and check its result;
5. execute the code.

Both ARM and Thumb probes completed two write/execute cycles. The first generated function returned 42; the rewritten function returned 43. Every `mprotect` and `msync` call returned zero.

```text
executable_memory=PASS
executable_memory_arm_exit=0
executable_memory=PASS
executable_memory_thumb_exit=0
```

Direct RWX variants were removed because the working writable-to-executable sequence is sufficient and has narrower permissions.

## Hardware TLS-register contract

Read-only sampling across four simultaneous threads established:

- Native QNX `__tls()` is stable, distinct per thread, and stable through signals.
- TPIDRURW remained zero across 20,000 scheduling yields per thread.
- TPIDRURO repeatedly changed between small values during scheduling, with roughly 10,000 changes per 20,000 samples. It is not a stable per-thread guest TLS location.
- Native QNX TLS is not stored in either user CP15 TLS register on this device.

The controlled TPIDRURW write probe then installed a distinct marker in four simultaneous threads. It failed in the important way:

- markers bled between pthreads after scheduling;
- workers sometimes started with another worker's marker;
- signal entry sometimes observed another worker's marker;
- TPIDRURW is writable but is not context-switched per pthread by this QNX build.

The historical scheme that rewrote guest TPIDRURO reads to TPIDRURW is therefore unsafe for concurrent guest threads.

The probe left markers on CPU 0 and CPU 2. `tpidrurw-cleanup` pinned one thread to each of four CPUs using `_NTO_TCTL_RUNMASK`, wrote zero, and verified zero on every CPU:

```text
cpu=0 threadctl_result=0 errno=0 before=4c580101 after=0
cpu=1 threadctl_result=0 errno=0 before=0 after=0
cpu=2 threadctl_result=0 errno=0 before=4c580102 after=0
cpu=3 threadctl_result=0 errno=0 before=0 after=0
tpidrurw_cleanup=PASS cpus=4
```

The SDK exposes no TLS-related `ThreadCtl` command that would opt TPIDRURW into kernel context switching.

## Trap-emulated guest TLS reads

The safe fallback was tested by replacing a guest TLS read with an undefined instruction. The SIGILL handler identifies the native pthread through `__tls()`, writes that thread's guest TLS value into saved guest register `r0`, advances saved PC to an explicit resume symbol, and returns.

Four simultaneous threads completed 1,000 emulated reads each in both ARM and Thumb modes:

```text
emulated_tls_read=PASS threads=4 reads_per_thread=1000 unexpected_traps=0
emulated_tls_read_arm_exit=0
emulated_tls_read=PASS threads=4 reads_per_thread=1000 unexpected_traps=0
emulated_tls_read_thumb_exit=0
```

This is the current correct design direction: patch and trap guest TLS-register reads, then return the per-thread guest TLS value through saved register state. Do not store guest TLS persistently in TPIDRURW.

## Repository changes in this session

- Added `native-probes/hello.c`.
- Added `native-probes/thread-signal-stress.c`.
- Added `native-probes/instruction-trap.c`.
- Added `native-probes/executable-memory.c`.
- Added `native-probes/tls-registers.c`.
- Added `native-probes/tpidrurw-switch.c`.
- Added `native-probes/tpidrurw-cleanup.c`.
- Added `native-probes/emulated-tls-read.c`.
- Added `scripts/build-native-probes.sh`.
- Added `scripts/run-native-probes.sh`.
- Updated `root-analysis/bb10-native-baseline.c` only to replace an old-GCC initializer warning with an explicit `memset`.

Device sources and build outputs are under `/accounts/1000/shared/misc/linuxemu-dev`. The verified SDK is already extracted there; it does not need to be transferred again.

## Minimal Linux execution core

The rebuild now includes `src/linuxemu.c`, a deliberately narrow first execution core. It:

- validates ELF32 little-endian ARM `ET_EXEC` files;
- rejects interpreters and load ranges outside the initial guest window;
- maps `PT_LOAD` segments at their requested addresses;
- tracks and replaces exact ARM `svc #0` words in executable segments;
- changes mappings to final permissions and synchronizes executable pages with `MS_INVALIDATE_ICACHE`;
- installs a `SA_SIGINFO` SIGILL dispatcher;
- creates a separate one-megabyte guest stack with `argc`, `argv`, `envp`, and core Linux auxiliary vectors, then enters through an ARM trampoline;
- translates Linux ARM `write`, `exit`, and `exit_group`;
- returns Linux `-ENOSYS` for unsupported calls.

Four synthetic static ARM guests are built from source with the recovered SDK. The fourth validates 16-byte stack alignment, forwarded arguments, environment traversal, and `AT_PHDR`, `AT_PHENT`, `AT_PHNUM`, `AT_PAGESZ`, `AT_ENTRY`, `AT_RANDOM`, and `AT_EXECFN`. The bounded smoke suite passed:

```text
=== guest: write-exit ===
linuxemu: entry=100000 segments=2 patches=2 stack=104bad90
hello from Linux ARM guest
write-exit exit=0 expected=0
=== guest: unknown-syscall ===
linuxemu: entry=100000 segments=1 patches=2 stack=105ead90
unknown-syscall exit=0 expected=0
=== guest: exit-status ===
linuxemu: entry=100000 segments=1 patches=1 stack=1066cd90
exit-status exit=37 expected=37
=== guest: initial-stack ===
linuxemu: entry=100000 segments=1 patches=1 stack=1042dd70
initial-stack exit=0 expected=0
linuxemu_smoke_failures=0
```

This is application-level evidence for the first static execution path, not a general ELF loader. At this checkpoint, dynamic musl loading was the next milestone.

## Initial dynamic musl execution

The pinned dynamic fixture is the official Alpine 3.24.2 armhf minirootfs:

```text
file=alpine-minirootfs-3.24.2-armhf.tar.gz
sha256=d86af88b58954d8a90c211004f1e61f208a62d1bb5a2f525f0c1cc26408ab92b
interpreter=/lib/ld-musl-armhf.so.1
```

The loader now accepts PIE main executables and their `PT_INTERP`, maps them at separate fixed biases, supplies `AT_BASE`, and begins execution at the interpreter entry while keeping the main entry in `AT_ENTRY`. The executable patch pass handles both ARM `svc #0` and direct TPIDRURO reads. The latter return the current guest TLS pointer; Linux ARM `set_tls` updates it.

Musl also probes the Linux ARM kuser page, which does not exist on QNX. The signal dispatcher narrowly emulates the kuser version word and the `get_tls`, memory-barrier, and 32-bit compare-exchange helper entries. Linux `mmap2` and `mprotect` protection flags are translated explicitly because Linux uses bits `1/2/4` while this QNX ABI uses `0x100/0x200/0x400`. A native subrange probe confirms QNX can change a single page within a two-page mapping for `PROT_NONE` to read/write, read/write to `PROT_NONE`, and read/write to read/execute.

The full native, static, and dynamic suites passed. The dynamic result was:

```text
=== dynamic guest: true ===
linuxemu: entry=2000fe48 start=60067b8c segments=4 patches=604 stack=1059dd40
true exit=0 expected=0
=== dynamic guest: echo ===
linuxemu: entry=2000fe48 start=60067b8c segments=4 patches=604 stack=10aaed30
dynamic-musl-ok
echo exit=0 expected=0
linuxemu_dynamic_smoke_failures=0
```

This establishes a real musl dynamic-linker path for the pinned BusyBox commands. General shared-library file mapping, rootfs path translation, guest threading, guest signals, decoded instruction boundaries, and the broader Linux syscall surface remain outstanding.
