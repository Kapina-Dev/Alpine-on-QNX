# Phase 4 part 2: Thumb runtime patching — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Libraries mapped at run time are patched ARM/Thumb/data by mapping symbol, `mprotect` no longer re-scans file pages, and Thumb signal handlers and Thumb entry points work.

**Architecture:** The static loader's mapping-symbol helpers (`load_mapping_symbols`, `patch_section_runs` in `src/elf_loader.c`) move to `src/arm_patch.c` behind two exported functions; `arm_patch_elf_mapping` (runtime `mmap`) uses them per executable section with a per-section bias. `linux_mprotect_runtime` patches only anonymous runs, and only under `LINUXEMU_PATCH_ANONYMOUS=1`. Signal delivery sets the CPSR T bit from bit 0 of the handler; the loader accepts `e_entry` bit 0 (`linuxemu_enter_guest` already uses `bx`).

**Tech Stack:** C (gnu99) for QNX 10.3 ARM, built on the Passport with `sdk/bin/gcc`; guest tests in ARM/Thumb-2 assembly (QNX SDK `as`/`ld`, `guest-tests/minimal-arm.ld`) and one musl fixture pair built with Alpine gcc under Linuxemu.

**Spec:** `docs/superpowers/specs/2026-11-23-phase4b-thumb-runtime-design.md`

## Global Constraints

- A file with no `.symtab` or no mapping symbols keeps today's ARM-only patching.
- Anonymous memory is never patched by default; `LINUXEMU_PATCH_ANONYMOUS=1` keeps its part 1 meaning.
- No regression for ARM-only guests: full device regression `linuxemu_full_regression_failures=0`; corpus 10/11 with the same apps.
- A `$d` run in a runtime-mapped file must stay byte-identical.
- Device: `ssh -o HostName=<device ip> bb10` (never write the IP into the repo); dev tree `/accounts/1000/shared/misc/linuxemu-dev`; check `sensors` before long runs; `run-full-regression` and `run-corpus` need `ssh -tt` with stdout a terminal.
- Tests that execute a possibly-unpatched `svc` set `ip` to `0x7fff` first (QNX ignores the svc immediate and runs the call named by `ip`).
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, no `Claude-Session:` trailer; grep added lines for `192\.168` before each commit.

## Review Focus

1. A mapping that starts in the middle of a Thumb run (an `mmap` at a nonzero page offset inside `.text`): the scan starts at the cut, so it may read the tail of a 32-bit instruction as a 16-bit one. Loaders map whole segments; Task 1's test maps a page that starts inside `.text` and the patched sites must all be real instructions (the test checks its known sites, and the `$d` word stays intact).
2. Mapping symbols from sections that are not executable, or with `st_shndx` out of range / `SHN_ABS`: they must be ignored, not biased into nonsense. Task 1 code rule: only symbols whose `st_shndx` names a `SHF_EXECINSTR` `SHT_PROGBITS` section are kept.
3. A mapping that covers a section only partly (end cut): runs clip to the mapping; Thumb runs end on a halfword, ARM runs on a word. Covered by Task 1's single-page map.
4. A non-ELF file mapped executable (or an ELF without sections) and then made executable by `mprotect`: after Task 2 nothing patches it. Accepted (spec Goal 2); Task 2's test pins that an ELF file page made executable by `mprotect` keeps the patches it got at `mmap`.
5. A signal handler in ARM state interrupting Thumb code (and the reverse): the T bit must be cleared for an ARM handler, and `sigreturn` must restore the interrupted state. Task 3's test runs a Thumb handler from ARM code and returns.

---

### Task 1: Runtime file mappings use mapping symbols

**Files:**
- Modify: `src/arm_patch.c` (gain the moved helpers; `arm_patch_elf_mapping` uses them)
- Modify: `src/elf_loader.c:91-182` (remove `load_mapping_symbols`, `patch_section_runs`; call the exported versions)
- Modify: `src/linuxemu.h` (prototypes)
- Create: `guest-tests/runtime-thumb-map.S`
- Modify: `scripts/build-native-probes.sh` (guest list), `scripts/run-linuxemu-smoke.sh`

**Interfaces:**
- Produces:
  - `struct mapping_symbol *arm_mapping_symbols_load(int fd, const Elf32_Shdr *sections, size_t section_count, uint64_t file_size, size_t *count)` — unbiased `st_value` addresses, sorted; only symbols in an executable `SHT_PROGBITS` section; returns 0 with `*count = 0` when there are none. Caller frees.
  - `int arm_patch_code_runs(uintptr_t start, size_t size, uintptr_t bias, const struct mapping_symbol *symbols, size_t count)` — patches `[start, start+size)`; the state at runtime address `a` is `mapping_state_at(symbols, count, a - bias, …)`; ARM runs word-aligned to `arm_patch_range`, Thumb runs halfword-aligned to `arm_patch_thumb_range`, data skipped. With `count == 0` the whole range is ARM. Returns 0 / -1.
  - Static loader: `bias = load_bias`. Runtime mapping, per section: `bias = section_runtime_start - sh_addr`, where `section_runtime_start = address + (sh_offset - offset)`.

- [ ] **Step 1: Write the failing test `guest-tests/runtime-thumb-map.S`.** Its `.text` page at file offset `0x1000` holds (with `.balign 4096` so it is page-aligned in the file): a Thumb function `thumb_svc` (`movw ip,#0x7fff; movs r7,#20; svc #0; bx lr` — getpid), a Thumb function `thumb_tls` (`mrc p15,0,r0,c13,c0,3; bx lr`), and a data word `0xef000000` placed with `.word` after a `.balign 4` (gas emits `$d`). `_start` (ARM): `set_tls` (`0xf0005`) to `0x12340000`; open `argv[0]`; `mmap2(0, 0x1000, PROT_READ|PROT_EXEC, MAP_PRIVATE, fd, 1)`; then checks, exit codes:
  - 71 open failed, 72 mmap failed;
  - 73 the mapped `$d` word is not `0xef000000`;
  - 74 the mapped `thumb_svc` svc halfword is not `0xde00` (checked before calling, so an unpatched svc never runs);
  - 75 the mapped `thumb_tls` first halfword is not `0xde00`;
  - 76 `blx` to mapped `thumb_svc|1` returns a value different from `getpid` called from ARM;
  - 77 `blx` to mapped `thumb_tls|1` returns other than `0x12340000`;
  - exit 0 otherwise.
  Add it to the guest list in `build-native-probes.sh` and `run_guest runtime-thumb-map 0` with `LINUXEMU_ROOT=/` in `run-linuxemu-smoke.sh` (same pattern as `runtime-exec-mixed`).

- [ ] **Step 2: Run it on the device, expect RED.** Sync, `sh scripts/build-native-probes.sh`, then `LINUXEMU_ROOT=/ build/linuxemu build/guest-tests/runtime-thumb-map; echo $?`. Expected: 73 (the ARM blind scan rewrites the `$d` word). Also `readelf -s` the binary shows `$t` and `$d` symbols.

- [ ] **Step 3: Implement.** Move the helpers into `arm_patch.c` with the Interfaces signatures; `elf_loader.c` calls them (`bias = load_bias`). In `arm_patch_elf_mapping`, load the symbols once, then for each overlapping executable section clip to the mapping and call `arm_patch_code_runs` with the section's bias.

- [ ] **Step 4: GREEN.** Same command, expected exit 0. Then `sh scripts/run-linuxemu-smoke.sh` on the device: 0 failures; host `tests/thumb-decode-test` still passes.

- [ ] **Step 5: Commit** "Patch runtime file mappings by mapping symbol".

### Task 2: `mprotect` stops re-scanning file pages

**Files:**
- Modify: `src/linux_syscall.c` (`linux_mprotect_runtime` exec branch: patch condition becomes `anonymous && patch_anonymous_memory`)
- Modify: `guest-tests/runtime-thumb-map.S` (second mode), `guest-tests/runtime-exec-mixed.S` header/expectations if they rely on re-scan
- Modify: `scripts/run-linuxemu-smoke.sh`

**Interfaces:**
- Consumes: Task 1's `runtime-thumb-map`.

- [ ] **Step 1: Failing test.** `runtime-thumb-map` with an argument (`argc == 2`): map the page `PROT_READ|PROT_WRITE` (Linuxemu copies it in and patches it by symbol), then `mprotect(PROT_READ|PROT_EXEC)` (exit 78 if that fails), then run the same checks 73–77. Add `run_guest runtime-thumb-map 0 flip` (with `LINUXEMU_ROOT=/`).
- [ ] **Step 2: RED.** Expected 73: the `mprotect` ARM re-scan rewrites the `$d` word.
- [ ] **Step 3: Implement** the condition change; keep the RW-for-icache-sync and final protection exactly as now.
- [ ] **Step 4: GREEN** for both modes; `run-linuxemu-smoke.sh` 0 failures (`runtime-exec-mixed` still passes: its file page was patched at `mmap`).
- [ ] **Step 5: Commit** "Stop re-patching file pages on mprotect".

### Task 3: Thumb signal handlers and restorers

**Files:**
- Modify: `src/guest_signal.c:266-279` (signal frame setup)
- Create: `guest-tests/signal-thumb.S`
- Modify: `scripts/build-native-probes.sh`, `scripts/run-linuxemu-smoke.sh`

- [ ] **Step 1: Failing test `guest-tests/signal-thumb.S`.** ARM `_start` installs, with `rt_sigaction` (174) and `SA_RESTORER|SA_SIGINFO`, a Thumb handler for SIGUSR1 (address `|1`) that stores 1 in a flag word, and a Thumb restorer (address `|1`) doing `movs r7,#173; svc #0` (rt_sigreturn). Raises it with `kill(getpid(), SIGUSR1)`, then checks the flag. Then installs an ARM handler for SIGUSR2 (no restorer flag) that stores 2, raised from a Thumb function (`blx`), returning to Thumb and back. Exits: 81 sigaction failed, 82 flag not 1, 83 flag not 2, 0 otherwise. A wrong-mode handler dies with SIGILL/SIGSEGV (exit ≥ 128).
- [ ] **Step 2: RED.** Expected: killed by a signal (exit 132 or 139), not 0.
- [ ] **Step 3: Implement** in the frame setup: `pc = handler & ~1`, CPSR T bit (`0x20`) set when `handler & 1`, cleared otherwise; `lr = restorer` unchanged (bit 0 kept, so the handler's `bx lr` interworks). Also clear the IT bits (CPSR `0x0600fc00`) for the handler's entry.
- [ ] **Step 4: GREEN**, exit 0; `run-signal-smoke` and `run-linuxemu-smoke` 0 failures.
- [ ] **Step 5: Commit** "Run Thumb signal handlers in Thumb state".

### Task 4: Thumb entry points

**Files:**
- Modify: `src/elf_loader.c:295` (drop the `e_entry & 1` rejection) and `src/elf_loader.c:422-424` (executable check on `entry & ~1`)
- Modify: `scripts/run-loader-negative-tests.sh:36` (remove the `thumb-entry` mode; `tests/elf-fixture-tool.c` mode stays unused or is removed)
- Create: `guest-tests/thumb-entry.S`
- Modify: `scripts/build-native-probes.sh`, `scripts/run-linuxemu-smoke.sh`

- [ ] **Step 1: Failing test `guest-tests/thumb-entry.S`:** `.thumb_func _start` that writes `thumb-entry ok\n` (Thumb `svc`, patched by the static loader via `$t`) and exits 0; `readelf -h` shows an odd entry. `run_guest thumb-entry 0`.
- [ ] **Step 2: RED.** Expected: Linuxemu refuses it (exit 126/127 from the ENOEXEC path).
- [ ] **Step 3: Implement.** `auxv AT_ENTRY` keeps the odd value (as Linux does).
- [ ] **Step 4: GREEN**, prints the line and exits 0; `run-loader-negative-tests` and `run-linuxemu-smoke` 0 failures.
- [ ] **Step 5: Commit** "Accept Thumb entry points".

### Task 5: A real Thumb library through musl's `dlopen`

**Files:**
- Create: `guest-tests/thumb-module.c`, `guest-tests/thumb-dlopen.c`
- Create: `scripts/build-thumb-fixtures.sh` (like `build-phase9-fixtures.sh`, but **not stripped**, module built `-O2 -fPIC -shared -march=armv7-a -mthumb -mfpu=vfp -mfloat-abi=hard`)
- Create: `guest-tests/fixtures/thumb-module-armhf.so`, `guest-tests/fixtures/thumb-dlopen-armhf`
- Modify: `scripts/run-loader-process-stress.sh` (one more case)

- [ ] **Step 1: Write the fixtures.** `thumb-module.c` exports `int thumb_getpid(void)` (inline asm Thumb `svc #0`, r7=20, ip=0x7fff), `void *thumb_tp(void)` (`__builtin_thread_pointer()`), and `const unsigned *thumb_data(void)` returning the address of a word emitted in `.text` with `asm(".word 0xef000000")` (gas `$d`). `thumb-dlopen.c` `dlopen`s argv[1] and exits 1 dlopen, 2 data word changed, 3 `thumb_getpid() != getpid()`, 4 `thumb_tp() != __builtin_thread_pointer()` (the ARM main reads TLS its own way), 0 otherwise. Build with the script on the device (`ALPINE_ROOT=build/fixture-rootfs`); `readelf -s` the module shows `$t`.
- [ ] **Step 2: RED** against the pre-part-2 core (`build/linuxemu-phase4a` saved before Task 1): expected 2 (data word rewritten) or a crash; on the current core expected 0.
- [ ] **Step 3: Add** the case to `run-loader-process-stress.sh` (copy module into the test rootfs `/tmp`, run with `--linuxemu-exec`, expect 0).
- [ ] **Step 4: GREEN**: `run-loader-process-stress` 0 failures.
- [ ] **Step 5: Commit** "Add a Thumb dlopen fixture".

### Task 6: Gate and docs

**Files:**
- Modify: `docs/FINDINGS.md` (Phase 4 part 2 entry), `docs/compatibility.md:499` (Thumb now patched in runtime-mapped files with symbols; stripped still ARM-only), `docs/capability-roadmap.md` (Phase 4 part 2 status after part 1's), `docs/corpus-results.md` (part 2 run), `docs/development.md` (new fixture script)

- [ ] **Step 1:** `sensors`, then full device regression via `ssh -tt`: expected `linuxemu_full_regression_failures=0`.
- [ ] **Step 2:** Corpus: expected 10/11, same apps.
- [ ] **Step 3:** Write the docs with the core's SHA-256 and results; commit "Record the Phase 4 part 2 gate".
