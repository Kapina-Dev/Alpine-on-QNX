# Phase 4 part 3: stripped-binary decoding — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Files without mapping symbols but with Thumb evidence are decoded function by function (unwind-table starts, evidence-propagated modes, literal pools and `tbb` tables as data) into synthesized mapping symbols, or refused when a possible patch site can't be classified.

**Architecture:** A new pure module `src/code_map.c` (no QNX/ELF dependencies, host-tested like `thumb_decode.c`) holds instruction facts and the per-function map builder. `src/arm_patch.c` gathers ELF evidence (`.ARM.exidx`, `.dynsym`, `.rel.dyn`, `.dynamic`, `e_entry`), runs the fixpoint, and returns synthesized symbols from `arm_mapping_symbols_load` when the file has none, so both loaders and `arm_patch_code_runs` are unchanged.

**Tech Stack:** C gnu99, QNX 10.3 ARM (built on the Passport); host gcc for unit tests; guest fixtures built with Alpine gcc under Linuxemu.

**Spec:** `docs/superpowers/specs/2026-11-23-phase4c-stripped-decoding-design.md`

## Global Constraints

- Files with mapping symbols, and files without Thumb evidence, are patched exactly as today.
- A file that loads has every possible patch site classified; otherwise `ENOEXEC` plus diagnostic `detail=undecodable-code`.
- Possible patch site = ARM word `0xef000000` or `(w & 0xffff0fff) == 0xee1d0f70` at word alignment; Thumb halfword `0xdf00`, or halfword pair `0xee1d, (h & 0x0fff) == 0x0f70` at halfword alignment.
- Gate: full device regression `linuxemu_full_regression_failures=0`; corpus 10/11, same apps.
- Device: `ssh -o HostName=<device ip> bb10` (never write the IP into the repo); `sensors` before long runs; `run-full-regression`/`run-corpus` need `ssh -tt`.
- Tests that may execute an unpatched `svc` set `ip` to `0x7fff` first.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`, no `Claude-Session:` trailer; grep added lines for `192\.168` before each commit.

## Review Focus

1. A literal pool referenced only backwards (Thumb-2 `ldr.w` with U=0, ARM negative offset) after the decoder already walked through it as code: the fixpoint must re-decode, and the final map must mark it data. Task 2 test: backward-referenced pool holding `0xdf00`.
2. `tbb`/`tbh` whose bound `cmp` is not in the look-back window, or uses a register different from the table index: decoding of that function stops; the rest is undecoded and refused only if it holds a pattern. Task 2 tests both.
3. A function whose mode stays unknown because it is reached only through a computed pointer not in relocations (e.g. built at run time): refused only if it holds a pattern. Task 2 test.
4. Thumb evidence from an `R_ARM_RELATIVE` word that is a data pointer which happens to be odd and lands in code (a `char *` into `.text` strings): must not create a bogus function start. Rule: an odd pointer only names a Thumb function if it equals an `.ARM.exidx` start + 1 or a `.dynsym` function. Task 3 test in the fixture (odd byte pointer into a code-section string).
5. `.plt`, `.init`, `.fini` and padding between functions: no exidx entries; they're ARM in GNU output. Rule: sections named `.plt` are ARM; other undecoded ranges follow Goal 3. Task 3 checks the stripped fixture loads (its `.init`/`.fini` hold no patterns).

---

### Task 1: Instruction facts (`src/code_map.c`, host-tested)

**Files:** Create `src/code_map.h`, `src/code_map.c`, `tests/code-map-test.c`.

**Interfaces — Produces:**
```c
enum insn_flow { FLOW_NEXT, FLOW_CALL, FLOW_BRANCH, FLOW_TABLE, FLOW_STOP };
struct insn_info {
    unsigned width;            /* 2 or 4 */
    enum insn_flow flow;
    int target_valid;          /* call/branch target known */
    uint32_t target;           /* absolute address */
    int target_thumb;          /* mode at target */
    int literal_valid;         /* reads PC-relative data */
    uint32_t literal;          /* absolute address of the data */
    unsigned literal_size;     /* 1, 2, 4 or 8 */
    unsigned table_reg;        /* FLOW_TABLE: index register; table_entry_size 1 or 2 */
    unsigned table_entry_size;
};
int thumb_insn_info(const uint16_t *code, size_t halfwords, uint32_t address, struct insn_info *out);
int arm_insn_info(uint32_t word, uint32_t address, struct insn_info *out);
/* For tbb/tbh bounds: if the instruction at `code` is `cmp rN, #imm`, set *reg and *imm. */
int thumb_cmp_imm(const uint16_t *code, size_t halfwords, unsigned *reg, uint32_t *imm);
```
Encodings to cover (each a test): Thumb `bl`, `blx imm` (target ARM, `Align(PC,4)`), `b` T2/T4 and conditional T1/T3 (same mode), `ldr` literal T1 (`0x48xx`) and T2 (`0xf8df`/`0xf85f`), `ldrb/ldrh/ldrsb/ldrsh` literal, `ldrd` literal (`0xe9df`/`0xe95f`), `vldr` literal (`0xed9f`/`0xed1f`/`0xeddf`/`0xed5f`, size 8 when bit 8 of hw2 set), `tbb`/`tbh` with Rn=pc (`0xe8df`, `0xf00m`/`0xf01m`), `cmp` T1 and T2; ARM `bl`, `blx imm` (target Thumb, +H*2), `b`, `ldr` literal (`(w & 0x0f7f0000) == 0x051f0000`), `ldrd` literal (`(w & 0x0f7f00f0) == 0x014f00d0`), `vldr` literal (`(w & 0x0f3f0e00) == 0x0d1f0a00`).

- [ ] Step 1: write `tests/code-map-test.c` with one check per encoding above (expected targets/literal addresses computed by hand in the test comments).
- [ ] Step 2: RED: `gcc -std=gnu99 -Isrc tests/code-map-test.c src/code_map.c src/thumb_decode.c` fails (missing functions).
- [ ] Step 3: implement.
- [ ] Step 4: GREEN: `code_map_test_failures=0`; `thumb_decode_test_failures=0`.
- [ ] Step 5: commit "Decode Thumb and ARM control flow and literal loads".

### Task 2: The map builder (pure, host-tested)

**Files:** Modify `src/code_map.c/.h`, `tests/code-map-test.c`.

**Interfaces — Produces:**
```c
enum fn_mode { MODE_UNKNOWN, MODE_ARM, MODE_THUMB };
struct code_function { uint32_t start; uint32_t end; enum fn_mode mode; };
struct code_map_result {
    struct mapping_symbol *symbols; size_t symbol_count;   /* sorted $a/$t/$d */
    int refused; uint32_t refused_address;
};
/* Decode [base, base+size) of one executable section. functions: sorted,
   non-overlapping, ends set; modes are propagated in place (calls and
   branches into other functions set their mode; iterate to a fixpoint;
   a conflicting mode makes that function MODE_UNKNOWN). Undecoded bytes
   ($d) holding a possible patch site in either mode set refused. */
int code_map_build(const uint8_t *bytes, uint32_t base, size_t size,
    struct code_function *functions, size_t function_count,
    struct code_map_result *out);   /* 0, or -1 on allocation failure */
void code_map_result_free(struct code_map_result *result);
```
Rules: literal targets inside the function become data and the function is re-decoded until no new data appears (max 8 rounds); a `tbb`/`tbh` is bounded by a `cmp` of its index register within the previous 4 instructions (entries = imm + 1; table bytes rounded up to a halfword), otherwise decoding of the function stops at the table (rest undecoded).

- [ ] Step 1: tests, each a small hand-assembled buffer: (a) Thumb function with a forward pool holding `0xdf00` → pool is `$d`, the real `svc` is in a `$t` run, not refused; (b) backward-referenced pool (focus 1); (c) bounded `tbb` table containing `0xdf00` bytes → `$d`; (d) unbounded `tbb` followed by an `svc` → refused at that address; (e) unbounded `tbb` followed by no pattern → not refused; (f) two functions, the first `bl`s the second, second's mode unknown → becomes Thumb; (g) unknown-mode function with an ARM `svc` word → refused (focus 3); (h) same without any pattern → not refused; (i) ARM function with `ldr` literal of `0xef000000` → `$d`.
- [ ] Step 2: RED (functions missing).
- [ ] Step 3: implement.
- [ ] Step 4: GREEN; both host tests 0 failures.
- [ ] Step 5: commit "Build code maps for functions without mapping symbols".

### Task 3: ELF evidence and loader integration

**Files:** Modify `src/arm_patch.c` (`arm_mapping_symbols_load` falls back to synthesis), `src/linuxemu.h`; Create `guest-tests/stripped-mixed-module.c`, `guest-tests/stripped-mixed-arm.c` (the ARM-mode object), `guest-tests/stripped-compare.c`, `scripts/build-stripped-fixtures.sh`; fixtures `guest-tests/fixtures/stripped-mixed-armhf.so`, `stripped-mixed-armhf.stripped.so`, `stripped-compare-armhf`; Modify `scripts/run-loader-process-stress.sh`.

**Interfaces — Consumes:** Task 2's `code_map_build`. **Produces:** `arm_mapping_symbols_load` returns synthesized symbols (and `-1` with `errno = ENOEXEC` on refusal, after `linux_diag_record(0, 0, "undecodable-code", {address, ...})`) when the file has no mapping symbols and has Thumb evidence.

Evidence: `.dynsym` `STT_FUNC` values; `e_entry`; `DT_INIT`/`DT_FINI`; `.rel.dyn` `R_ARM_RELATIVE` words read from the file — an odd value names a Thumb function only if `value - 1` is an exidx start or a `.dynsym` function (focus 4); an even value names an ARM function only if it equals an exidx start. Functions: exidx starts (`prel31`, last entry's end = section end). Sections named `.plt`: ARM.

- [ ] Step 1: fixtures. `stripped-mixed-module.c` (Thumb, `-funwind-tables`): ≥ 20 functions — exported ones with inline Thumb `svc` (getpid, `ip=0x7fff`) and TLS reads, static ones reached only by `bl` or only through a function-pointer table, `switch` statements dense enough for `tbb`/`tbh`, constants `0xef000000` and `0xdf00df00` loaded from pools, an odd `char *` into a string in `.text` (focus 4). `stripped-mixed-arm.c` (`-marm`): ARM functions with an inline ARM `svc`, called from the Thumb side. Built into one library; the build script keeps an unstripped copy and a `strip`ped copy. `stripped-compare.c` `dlopen`s both (different paths), finds each copy's executable `PT_LOAD` via `dl_iterate_phdr`, compares them byte for byte (exit 2 if they differ, printing the first offset), then calls every exported function of the stripped copy (exit 3 on a wrong result). Add the case to `run-loader-process-stress.sh` (expect 0).
- [ ] Step 2: RED on the current core: the comparison differs (exit 2): the stripped copy is ARM-scanned.
- [ ] Step 3: implement the evidence gathering and fallback.
- [ ] Step 4: GREEN: exit 0; refusal case: a copy of the stripped library whose one function's mode is made unknowable (the build script drops its `.dynsym` entry by building it `static` and only reaching it through a pointer computed at run time) and holds an `svc` → `dlopen` fails, `LINUXEMU_DIAGNOSTICS` report has `detail=undecodable-code`; `run-loader-process-stress`, `run-linuxemu-smoke`, `run-dynamic-smoke` 0 failures.
- [ ] Step 5: commit "Decode stripped libraries with Thumb code".

### Task 4: Stripped Thumb program (program loader)

**Files:** Modify `scripts/run-linuxemu-smoke.sh`, `scripts/build-native-probes.sh` (a stripped copy of `thumb-entry` built with `--strip-all`, plus an `.ARM.exidx` entry via `.fnstart`/`.fnend`).

- [ ] Step 1: `thumb-entry-stripped` (the `thumb-entry` source with `.fnstart/.cantunwind/.fnend`, linked then `strip`ped).
- [ ] Step 2: RED: today its Thumb `svc` is not patched; it must not run unpatched, so the test first checks (via `LINUXEMU_VERBOSE` load line or the exit) — expected: refused or wrong exit. (If today's core loads it and runs the Thumb `svc` unpatched, `ip=0x7fff` makes that an invalid QNX call: exit non-zero.)
- [ ] Step 3: no new code expected (Task 3 covers the loader); fix if not.
- [ ] Step 4: GREEN: prints `thumb-entry ok`, exit 0.
- [ ] Step 5: commit "Run a stripped Thumb program".

### Task 5: SQLite as a real-world check

**Files:** Create `scripts/build-sqlite-thumb-fixture.sh` (downloads the pinned SQLite amalgamation zip, checks its SHA-256 from `docs/corpus/sqlite-amalgamation.sha256`, builds `libsqlite3.so` `-O2 -mthumb -march=armv7-a -funwind-tables` unstripped and stripped on the device; the fixtures are not committed if over 2 MB — they're built into `build/`); Modify `scripts/run-loader-process-stress.sh` (run the `stripped-compare` program on the SQLite pair when present, then a small SQL round trip through the stripped copy).

- [ ] Step 1: build script and pin.
- [ ] Step 2: run on the Task 2 core (before Task 3 is applied, using the saved `build/linuxemu-phase4b`): comparison differs.
- [ ] Step 3: on the current core: identical; SQL round trip prints the expected row. If refused, record the refusal address and the cause in the ledger and FINDINGS (a real-world coverage gap) and fix the decoder only if the cause is a pattern the design covers.
- [ ] Step 4: commit "Check stripped decoding against SQLite".

### Task 6: Gate and docs

**Files:** `docs/FINDINGS.md` (part 3 entry, the exidx-has-no-Thumb-bit fact, the section-header correction), `docs/capability-roadmap.md` (part 3 status; correct "ship without section headers"), `docs/compatibility.md` (stripped files: decoded with Thumb evidence, else ARM; refusal rule), `docs/corpus-results.md`, `docs/development.md` (new fixture scripts).

- [ ] Step 1: `sensors`; full device regression via `ssh -tt`: `linuxemu_full_regression_failures=0`.
- [ ] Step 2: corpus: 10/11, same apps.
- [ ] Step 3: docs with the core's SHA-256; commit "Record the Phase 4 part 3 gate".
