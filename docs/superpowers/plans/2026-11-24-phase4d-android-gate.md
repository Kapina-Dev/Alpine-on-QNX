# Phase 4 part 4: the Android gate — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Android 6.0's linker and libraries (API 23 emulator image) load under Linuxemu with every patch site found and nothing refused. Part 3's wrong-mode bug is fixed, coverage is extended and measured on the laptop, and the device runs a dynamic and a static bionic test.

**Architecture:** Decoder changes stay in the pure module `src/code_map.c` (host-tested by `tests/code-map-test.c`). A new host tool, `tests/code-map-survey.c`, measures a whole `/system` tree and prints the expected patch-site count per file. The runtime gains one `LINUXEMU_VERBOSE` line per patched file mapping, so device counts can be compared with the laptop's. Fetch, build and run scripts keep every Google/NDK binary under the ignored `build/`.

**Tech Stack:** C gnu99, QNX 10.3 ARM (built on the Passport); host gcc for unit tests and the survey; NDK r27c clang for the bionic test programs; `debugfs` for the image.

**Spec:** `docs/superpowers/specs/2026-11-24-phase4d-android-gate-design.md`

## Global Constraints

- Image: `armeabi-v7a-23_r06.zip` SHA-1 `7cf2ad756e54a3acfd81064b63cb0cb9dff2798d`, URL `https://dl.google.com/android/repository/sys-img/android/armeabi-v7a-23_r06.zip`. NDK: `android-ndk-r27c-linux.zip` SHA-1 `090e8083a715fdb1a3e402d0763c388abb03fb4e`, URL `https://dl.google.com/android/repository/android-ndk-r27c-linux.zip`. Both unpack under `build/android/` (`build/android/system`, `build/android/android-ndk-r27c`). Never commit them or anything extracted from them.
- Files with mapping symbols, and files without Thumb evidence, are patched exactly as today. Any `$a`/`$t`/`$d` in `.symtab` → mapping-symbol path.
- Refusal stays the answer for unclassifiable patch sites (`ENOEXEC`, diagnostic `detail=undecodable-code`). No mode guessing from bytes beyond the fixed stub sequence of Task 2.
- Possible patch site: ARM word `0xef000000` or `(w & 0xffff0fff) == 0xee1d0f70` at word alignment in ARM runs; in Thumb runs, what `thumb_scan` reports.
- No Phase 5/6 work (pids, tids, procfs) to get the dynamic test further.
- Gate: full device regression `linuxemu_full_regression_failures=0`; corpus 10/11, the same apps.
- Device: `ssh -o HostName=<device ip> bb10` (never write the IP into the repo); `sensors` before long runs; `run-full-regression`/`run-corpus` need `ssh -tt`; don't pipe data into `ssh -tt`.
- Tests that may execute an unpatched `svc` set `ip` to `0x7fff` first.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, no `Claude-Session:` trailer; grep added lines for `192\.168` before each commit. Stage only this part's files (the user's uncommitted files stay out).
- Laptop disk is tight: scripts delete archives right after unpacking.

## Review Focus

1. **Patch table overflow:** the dynamic test maps the linker plus 10 libraries, and `MAX_PATCHES` is 4096 (`src/linuxemu.h:20`). If the survey's total for those 11 files is near or above it, the load fails with `E2BIG`, not a refusal. Task 1 prints the total. Task 6's run asserts no `E2BIG` and reports the total. If it's over 2048, raise `MAX_PATCHES` in Task 6 and say so in the commit.
2. **A `bx pc; nop` pair that isn't a stub** (halfword-aligned but not word-aligned, or not followed by valid ARM code): it must not start an ARM function at a misaligned address. Task 2 test: the same halfwords at `A % 4 == 2` change nothing.
3. **PC-relative formation through a clobbered register:** `ldr r3, =off` followed by `mov r3, r0` and then `add r3, pc` must not produce evidence. Task 4 test.
4. **PC-relative result landing in data or outside code** (a string address formed the same way): it gives no evidence and changes nothing. Task 4 test with the formed address in `.rodata` and inside a literal pool.
5. **`.symtab` `STT_FUNC` sizes or values that point at data** (symbols for assembly data marked `%function`): `.symtab` evidence is trusted like `.dynsym`, so an odd value can only mark a function start. The stripped-libc pair (Task 2) and the linker survey (Task 3) must still show 0 data decoded as code.

---

### Task 1: Fetch scripts and the laptop survey

**Files:** Create `scripts/fetch-android-sysimg.sh`, `scripts/fetch-android-ndk.sh`, `tests/code-map-survey.c`. Modify `docs/development.md` (Stripped-binary section: survey command, fetch scripts).

**Interfaces — Produces:**
- `build/code-map-survey DIR...` prints, for each ELF file under DIR (recursive, regular files, `EM_ARM` only), one line:
  `survey file=<path relative to DIR> mapsyms=<0|1> thumb=<0|1> refused=<0|1> at=0x<addr> unsettled=<0|1> sites=<N>`
  and finally `survey_total files=<n> refused=<n> nothumb=<n>`. `refused`, `at` and `unsettled` always come from `code_map_from_elf` (it ignores `.symtab`), so they describe the file as if fully stripped. `sites` is the number of possible patch sites in executable sections under the file's own mapping symbols when `mapsyms=1`, else under `code_map_from_elf`'s map (`-1` when refused). With `--pairs`, every `mapsyms=1` file is also checked against a copy with `.symtab` removed in memory (`code_map_from_elf` reads without `.symtab`, so passing the unstripped image is the same input), with the `compare_pair` counts printed as `pair file=<path> wrong_mode=<n> data_as_code=<n> undecoded=<n>`.
- `compare_pair` and the site-counting helpers move from `tests/code-map-test.c` to a shared `tests/code-map-compare.c` / `.h`, used by both tools.
- `scripts/fetch-android-sysimg.sh [OUT]` (default `build/android/system`): download, check SHA-1, `debugfs -R "rdump /lib /bin /build.prop OUT"`, delete zip and image. Exit 0 if `OUT/build.prop` already holds fingerprint `Android/sdk_phone_armv7/generic:6.0/MASTER/3079352:userdebug/test-keys`.
- `scripts/fetch-android-ndk.sh [DIR]` (default `build/android`): exit 0 if `DIR/android-ndk-r27c/source.properties` has `Pkg.Revision = 27.2.12479018`, else download, check SHA-1, unzip, delete zip.

- [ ] **Step 1:** Move `compare_pair` and the patch-site counting out of `tests/code-map-test.c` into `tests/code-map-compare.c`. Run `build/code-map-test`: still `code_map_test_failures=0`.
- [ ] **Step 2:** Write `tests/code-map-survey.c`. Build it with `gcc -std=gnu99 -O2 -Isrc -Itests -o build/code-map-survey tests/code-map-survey.c tests/code-map-compare.c src/code_map.c src/thumb_decode.c`.
- [ ] **Step 3: Baseline.** Run `build/code-map-survey --pairs build/android/system > build/android/survey-before.txt`. Expected (the design's facts): `bin/linker` `refused=1 at=0xf1ba`; under `lib/`, 7 of 218 refused when decoding ignores symbols; `lib/libc.so` and `lib/libart.so` `mapsyms=1`; `pair file=lib/libc.so wrong_mode=414 data_as_code=0`; `lib/libm.so thumb=0`. Print the sum of `sites` over the linker and the 10 libraries (`libc libm libutils libdl libbacktrace libcutils liblog libc++ libbase libunwind`) for Review Focus 1.
- [ ] **Step 4:** Write both fetch scripts. Check them: `sh scripts/fetch-android-ndk.sh` exits 0 straight away (NDK present). `sh scripts/fetch-android-sysimg.sh /tmp/claude-…/scratchpad/sys` (a scratch dir) downloads and extracts; `diff -r` against `build/android/system` is empty; then delete the scratch copy.
- [ ] **Step 5: Commit** "Survey a whole Android tree with the stripped-code decoder" (survey, compare split, fetch scripts, development.md). Paste the Step 3 baseline numbers into the commit body.

### Task 2: Thumb→ARM interworking stubs

**Files:** Modify `src/code_map.c` (mode propagation / span building), `tests/code-map-test.c`.

**Interfaces:** none new. Rule: at a Thumb call or branch target `A` with `A % 4 == 0` and halfwords `0x4778, 0x46c0` (`bx pc; nop`), the span `[A, A+4)` is Thumb and an ARM function starts at `A+4` (strong, like a direct call target). This holds whatever exidx range or function `A` falls inside.

- [ ] **Step 1: Failing tests** in `test_map_builder` (hand-assembled bytes; follow the existing builder tests' style):
  - `stub: thumb bl to bx-pc stub after an ARM function`: an ARM function `[0x1000,0x1010)` (exidx-style start, `MODE_ARM`), then at `0x1010`: `4778 46c0`, ARM `e59fc000` (`ldr ip,[pc]`), `e08cf00f` (`add pc, ip, pc`), data word `0xef000000`; a Thumb function elsewhere whose `bl` targets `0x1010`. Expect symbols `$t@0x1010`, `$a@0x1014`, `$d@0x101c`, not refused (the data word is a pattern and must sit in `$d`).
  - `stub: misaligned bx pc is not a stub` (Review Focus 2): the same halfwords at `0x1012`. Expect no `$a` at `0x1016`.
- [ ] **Step 2:** Build and run `build/code-map-test`. Expected: the two new checks FAIL.
- [ ] **Step 3:** Implement the rule in `src/code_map.c`.
- [ ] **Step 4:** `build/code-map-test`: `code_map_test_failures=0`. Then `build/code-map-survey --pairs build/android/system | grep '^pair'`: `lib/libc.so wrong_mode=0 data_as_code=0`, and the same for `lib/libart.so`. If `libart` still shows wrong-mode bytes, look at the first one: if it's the mirror ARM→Thumb stub, add the mirror rule with its own test, then rerun.
- [ ] **Step 5: Commit** "Decode Thumb-to-ARM interworking stubs as Thumb".

### Task 3: `.symtab` functions as evidence

**Files:** Modify `src/code_map.c` (`gather`), `tests/code-map-test.c`. Create `guest-tests/fixtures/stripped-mixed-armhf.nomap.so` (committed; our own fixture), made by `arm-linux-gnueabihf-objcopy --wildcard --strip-symbol='$*' guest-tests/fixtures/stripped-mixed-armhf.so guest-tests/fixtures/stripped-mixed-armhf.nomap.so`. Add that command to `scripts/build-stripped-fixtures.sh`.

**Interfaces:** `gather` reads `SHT_SYMTAB` (type 2) like `SHT_DYNSYM`: each `STT_FUNC` with a defined section adds `add_pointer` to both `modes` and `starts`. It does this only when that `.symtab` has no mapping symbols (`mapping_symbol_state` on each name, using `sh_link`'s string table).

- [ ] **Step 1: Failing test** in `test_fixture_files`: `compare_pair("guest-tests/fixtures/stripped-mixed-armhf.so", "guest-tests/fixtures/stripped-mixed-armhf.nomap.so", 0)`, so the no-mapping-symbol copy must match the unstripped symbols byte for byte. Also check that the nomap copy's `code_map_from_elf` result is not refused. (Before the change it either passes because `.dynsym`/exidx already cover the fixture, or fails. If it passes, add a static Thumb function with no exidx entry and no `.dynsym` entry to the fixture source so the `.symtab` symbol is its only evidence, rebuild the fixtures, and confirm RED.)
- [ ] **Step 2:** Run it, expect FAIL.
- [ ] **Step 3:** Implement the `SHT_SYMTAB` evidence.
- [ ] **Step 4:** `build/code-map-test`: 0 failures. Survey: `bin/linker refused=0`; libc/libart pairs still 0 wrong-mode and 0 data-as-code (Review Focus 5).
- [ ] **Step 5: Commit** "Use .symtab functions when a file has no mapping symbols".

### Task 4: PC-relative address formation

**Files:** Modify `src/code_map.h` (`struct insn_info`), `src/code_map.c` (`thumb16_info`, `thumb32_info`, `arm_insn_info`, `decode_thumb`, `decode_arm`), `tests/code-map-test.c`.

**Interfaces — Produces** (new `insn_info` fields, zero when not applicable):
```c
unsigned dest_reg;      /* register written by a literal load, movw or movt; 16 = none */
int pc_add;             /* add Rd, pc (Thumb T2 0x4478 form; ARM add Rd, pc, Rd / add Rd, Rd, pc) */
unsigned pc_add_reg;    /* its Rd (== Rn) */
int movw, movt;         /* movw/movt Rd, #imm16 (Thumb T3/T1, ARM A2/A1) */
uint16_t imm16;
```
Decoding tracks, per function walk, the last known value of each register from a 4-byte literal load (the loaded word, read from the section) or a `movw`/`movt` pair. Any other instruction that writes the register, any branch target and any call clear it. On `pc_add` with a known value `v`, the formed address is `v + PC`, where PC is `address + 4` in Thumb and `address + 8` in ARM. If it lands in this section, outside bytes already known as data, it's handled like a direct call target of mode `bit0 ? Thumb : ARM` (address without bit 0), but **weak**.

- [ ] **Step 1: Failing tests.** In `test_insn_info`, one check per new encoding: Thumb `add r3, pc` (`0x447b`), Thumb `movw r3,#0x1234` / `movt r3,#0x5678`, ARM `add r3, pc, r3` (`0xe08f3003`), ARM `movw`/`movt`; `dest_reg` on Thumb `ldr r3,[pc,#n]` (`0x4bxx`) and ARM `ldr r3,[pc,#n]`. In `test_map_builder`:
  - `pcrel: ldr/add pc names a Thumb function`: a Thumb function doing `ldr r3, [pc, #k]`; `add r3, pc`; `blx r3`; `bx lr`, with a pool word `= target - (add address + 4) | 1`. The target is a Thumb function reached by nothing else and holds `df00`. Expect `$t` at the target and not refused (before the change: refused).
  - `pcrel: clobbered register gives nothing` (Review Focus 3): `mov r3, r0` between the load and the add. Expect the target undecoded, refused.
  - `pcrel: formed address in data gives nothing` (Review Focus 4): the formed address points into the function's own literal pool. Expect the pool still `$d` and the symbols unchanged from a run without the add.
  - `pcrel: movw/movt pair, ARM`: the same as the first case in ARM mode with `movw`/`movt`, ARM target.
- [ ] **Step 2:** Run, expect the new checks to FAIL.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** `build/code-map-test`: 0 failures. Survey `--pairs`: libc/libart pairs 0 wrong-mode and 0 data-as-code; the survey's refused count is ≤ the Task 3 count. Save the output as `build/android/survey-after.txt`. Every file in the gate's 11 (linker plus the 10 libraries) is `refused=0`.
- [ ] **Step 5: Commit** "Follow PC-relative function addresses when decoding stripped code" (body: refusals before/after under `lib/` and `bin/`).

### Task 5: Report patch sites per file mapping

**Files:** Modify `src/arm_patch.c` (`arm_patch_elf_mapping`), `src/elf_loader.c` (where the main program and the interpreter are patched), `scripts/run-loader-process-stress.sh` (one check).

**Interfaces — Produces:** with `LINUXEMU_VERBOSE` set, each patched ELF file mapping and each loader-patched file prints one line to stderr:
`linuxemu: patch-sites ino=<st_ino> offset=0x<file offset> length=0x<length> sites=<n>`
where `n` is `arm_patch_count()` after minus before. Nothing is printed when the variable is unset.

- [ ] **Step 1: Failing check** in `run-loader-process-stress.sh`: run the existing stripped mixed fixture comparison with `LINUXEMU_VERBOSE=1`, sum `sites=` over the lines whose `ino` is the stripped library's (`ls -i` inside the rootfs), and compare with `build/code-map-survey`'s `sites` for `guest-tests/fixtures/stripped-mixed-armhf.stripped.so`, run on the laptop and written into the script as a constant with a comment saying how to regenerate it.
- [ ] **Step 2:** On the device, build the core and run the suite. Expected: the new check FAILS (no lines).
- [ ] **Step 3:** Implement the line in both places.
- [ ] **Step 4:** The suite passes on the device.
- [ ] **Step 5: Commit** "Report patch sites per file mapping under LINUXEMU_VERBOSE".

### Task 6: The device gate

**Files:** Create `guest-tests/android-gate.c`, `guest-tests/android-syscall.c`, `scripts/build-android-gate.sh` (laptop), `scripts/run-android-gate.sh` (device). Modify `docs/development.md` (how to build, upload and run the gate).

**Interfaces:**
- `guest-tests/android-gate.c` (dynamic): `main` writes `android-gate main\n` with `printf`, checks `syscall(__NR_getpid) == getpid()`, writes `android-gate syscall\n` with `syscall(__NR_write, 1, …)`, returns 0. Exit codes 2/3 mark which check failed.
- `guest-tests/android-syscall.c` (static, `-nostartfiles`, Thumb `_start`): set `ip` to `0x7fff`; call `syscall(__NR_write, 1, "android-syscall write\n", 22)`; check that `syscall(__NR_getpid)` is greater than 0; make one inline Thumb `svc #0` for `getpid` (`r7 = 20`) and compare; `syscall(__NR_exit_group, 0)`, or 2/3/4 per failed check.
- `scripts/build-android-gate.sh` (laptop, NDK r27c `toolchains/llvm/prebuilt/linux-x86_64/bin`):
  - dynamic: `armv7a-linux-androideabi23-clang -mthumb -O2 -Wl,--no-as-needed -L build/android/system/lib -lutils -lm -o build/android/gate/data/android-gate guest-tests/android-gate.c`;
  - static: `armv7a-linux-androideabi29-clang -mthumb -static -nostartfiles -O2 -o …/data/android-syscall guest-tests/android-syscall.c`, then `llvm-strip` both;
  - check `llvm-readelf -l` shows `PT_INTERP /system/bin/linker` (dynamic) and `-d` lists `NEEDED` `libutils.so`, `libm.so` and `libc.so`;
  - copy `bin/linker` and the 10 libraries into `build/android/gate/system/{bin,lib}`;
  - write `build/android/gate/expected-sites.txt` (`<name> <sites>` from the survey, and the two test programs);
  - tar to `build/android/android-gate.tar`.
- `scripts/run-android-gate.sh` (device; upload the tar to the dev tree's `build/` without `-tt`): unpack to `build/android-gate-rootfs`, then run each program with `LINUXEMU_ROOT=<that dir>`, `LINUXEMU_VERBOSE=1` and `LINUXEMU_DIAGNOSTICS=<file>`, the same way `run-loader-process-stress.sh` runs rootfs programs. Checks:
  1. no `undecodable-code` record, no `E2BIG`, no `CANNOT LINK EXECUTABLE`;
  2. per file, the summed `sites` of its `patch-sites` lines (inode → name via `ls -i`) equals `expected-sites.txt`, for every file that has lines; the linker and all 10 libraries must have lines;
  3. static: output `android-syscall write`, exit 0;
  4. dynamic: **target** output `android-gate main` and `android-gate syscall`, exit 0. Otherwise print the exit status, the last 20 trace lines from a `LINUXEMU_SYSTRACE=1` rerun and the diagnostics file, and report `android_gate_dynamic=fallback`. This passes only if checks 1–2 passed (the spec's accepted fallback).
  
  It prints `android_gate_failures=<n>` and `android_gate_dynamic=<main|fallback>`.

- [ ] **Step 1:** Write both test sources and `build-android-gate.sh`; run it on the laptop. Expected: the readelf checks pass and the tar exists.
- [ ] **Step 2:** Write `run-android-gate.sh`. Run it on the device with the **part 3 core** (`build/linuxemu` before this branch's changes, i.e. core `50b74a6d…`). Expected: FAIL on check 1 (`undecodable-code` for the linker). This is the RED for the whole part.
- [ ] **Step 3:** Run it with this branch's core. Expected: `android_gate_failures=0`. Record `android_gate_dynamic`, the patch-site total and, if it's the fallback, where start-up stopped. If Review Focus 1's total is over 2048, raise `MAX_PATCHES` here (and rerun).
- [ ] **Step 4: Commit** "Run Android's linker and libraries through the Phase 4 gate".

### Task 7: Regression gate and records

**Files:** Modify `docs/FINDINGS.md`, `docs/capability-roadmap.md` (Phase 4 part 4 status), `docs/development.md` if not done.

- [ ] **Step 1:** `sensors` (battery), then the full device regression in `ssh -tt`. Expected: `linuxemu_full_regression_failures=0`.
- [ ] **Step 2:** The corpus in `ssh -tt`. Expected: 10 of 11, the same apps as part 3.
- [ ] **Step 3:** Write a `FINDINGS.md` entry. Cover: the facts (image is 6.0, not 6.0.1; which libraries keep mapping symbols; 6.0 libc has no pid check), survey refusals before/after, the wrong-mode cause and fix, the device gate results (dynamic `main` or fallback, with the reason), core sha256, and a **"Modified bionic: evidence"** section. That section states that the decision is the user's and gives the gate's evidence: whether patching covered everything the linker loaded, and what a modified bionic would and wouldn't change. Also write the roadmap Phase 4 part 4 status block in the style of parts 1–3.
- [ ] **Step 4: Commit** "Record the Phase 4 part 4 gate". Then ask the user for the modified-bionic decision and record it in `FINDINGS.md` and the roadmap as a follow-up commit.
