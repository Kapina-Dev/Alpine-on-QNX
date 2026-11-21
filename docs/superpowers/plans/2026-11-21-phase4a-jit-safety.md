# Phase 4 part 1: JIT safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Anonymous guest memory that `mprotect` makes executable is no longer patched, so a JIT's data is never rewritten and flips are cheap.

**Architecture:** `linux_mprotect_runtime` splits the range into runs by the runtime mapping registry's `anonymous` flag: file-backed runs are patched as today, anonymous runs are not. A read-only check reports `svc`-like words in anonymous runs when `LINUXEMU_DIAGNOSTICS` is on, and `LINUXEMU_PATCH_ANONYMOUS=1` restores the old patching. The check lands first, so the regression and corpus can show what the change would affect before the default changes.

**Tech Stack:** C (gnu99) for QNX 8 ARM via the on-device SDK; ARM assembly guest tests; POSIX sh runners; Alpine 3.24 armhf guest packages.

**Spec:** `docs/superpowers/specs/2026-11-21-phase4a-jit-safety-design.md`

## Global Constraints

- Work on branch `phase4a-jit-safety` in worktree `.claude/worktrees/phase4a-jit-safety`, from master `862c21f`.
- Device: `ssh -o HostName=<ip> bb10` (ask the user for the IP; never write any IP into the repo; grep each staged diff for `192\.168` before committing). Never `-tt` when piping; `run-full-regression.sh` and `run-corpus.sh` need `ssh -tt` and a terminal on stdout.
- Dev tree on the device: `/accounts/1000/shared/misc/linuxemu-dev`. Sync: `tar -cf - src scripts guest-tests native-probes docs/corpus | ssh … 'cd DEV && tar -xf -'`. Build: `sh scripts/build-native-probes.sh`.
- Check the battery with `sensors` before any long device run; plug in if low.
- Never stage `root-analysis/native-probe-results-20260922.md`, `native-probes/unix-rights-pressure.c`, `root-analysis/terminal-debug-20261014/`. Never run `scripts/run-iopkt-stress.sh`.
- Commits end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`; no `Claude-Session:` trailer.
- Switch name: `LINUXEMU_PATCH_ANONYMOUS=1`. Diagnostic detail: `anon-exec-svc` on `mprotect` (Linux nr 125), one record per flip that finds a match.
- Patterns (same as `arm_patch_range`): ARM `svc #0` word `0xef000000`; ARM TPIDRURO read `(word & 0xffff0fff) == 0xee1d0f70`. No Thumb patterns.
- File-backed mappings' patching does not change. Anonymous `mmap` that is executable from the start stays unscanned.

## Review Focus

- A single `mprotect` range that spans an anonymous run and a file-backed run: the file-backed part is still patched, the anonymous part untouched (Task 4 test `runtime-exec-mixed`).
- A program that really generates an `svc` into anonymous memory, run without the switch: it must at least be visible with diagnostics on (Task 1 check, exercised in Task 4 by `runtime-exec-syscalls` run without the switch under diagnostics).
- `LINUXEMU_PATCH_ANONYMOUS=1` surviving `fork` and `execve` (Task 4 test runs the switch through an `execve`).
- RW→RX→RW→RX flips of the same anonymous page with the switch off: no patch-table growth and data intact every time (the 5000-flip loop in `jit-patch-hazard`).
- A `pread`-copied guest file mapping (host `MAP_ANON`, registered file-backed) flipped executable: still patched (Task 4 `runtime-exec-mixed` uses a file mapping made writable then executable).

---

### Task 1: The anonymous-executable diagnostic check

**Files:**
- Modify: `src/guest_memory.c` (new run query), `src/linuxemu.h` (prototype)
- Modify: `src/linux_syscall.c` (`linux_mprotect_runtime`, new static helper)
- Modify: `scripts/run-linuxemu-smoke.sh` (new diagnostics case)

**Interfaces:**
- Produces: `int guest_memory_runtime_anonymous_at(uintptr_t address, uintptr_t limit, uintptr_t *run_end);` returns 1 when the runtime mapping covering `address` is private anonymous, 0 otherwise (file-backed or untracked); sets `*run_end` to where that answer next changes, clipped to `limit`. Takes `runtime_mapping_lock`.
- Produces: `static int anonymous_patch_candidate(uintptr_t start, size_t length, uintptr_t *first);` in `src/linux_syscall.c`: returns 1 and the first matching word's address when any 4-byte-aligned word in the range matches the Global Constraints patterns. Read-only.

- [ ] **Step 1: Write the failing smoke case.** In `scripts/run-linuxemu-smoke.sh`, after the existing diagnostics cases, add `diag-anon-exec`: run `jit-patch-hazard` with `LINUXEMU_DIAGNOSTICS="$diag_dir/anon-exec.txt"`; require a line containing both `name=mprotect` and `detail=anon-exec-svc`; the guest's exit status is not checked here (it still reports exit 20 until Task 4). On failure call `diag_fail diag-anon-exec "no anon-exec-svc record"`.
- [ ] **Step 2: Sync, build, run `sh scripts/run-linuxemu-smoke.sh` on the device.** Expected: `diag-anon-exec` fails (no record).
- [ ] **Step 3: Implement `guest_memory_runtime_anonymous_at` and `anonymous_patch_candidate`, and call the check in `linux_mprotect_runtime`.** In the `PROT_EXEC` branch, before `arm_patch_range`, walk the range run by run; for each anonymous run, when diagnostics are enabled and `anonymous_patch_candidate` finds a word, `linux_diag_record(125, 0, "anon-exec-svc", args)` with the first address in `args[0]`. Patching itself is unchanged in this task. Check how `linux_diag_record`/the report format prints error 0 and keep the line greppable as above.
- [ ] **Step 4: Rebuild and rerun `run-linuxemu-smoke.sh`.** Expected: `diag-anon-exec` passes and `linuxemu_smoke_failures=0` (`jit-patch-hazard` runs only inside this diagnostics case so far).
- [ ] **Step 5: Commit** "Report svc-like words in anonymous memory made executable".

### Task 2: Measure what the change would affect

**Files:**
- Modify: `docs/FINDINGS.md` (new entry)

No code. The core from Task 1 still patches anonymous memory.

- [ ] **Step 1: Full regression with diagnostics.** On the device: `LINUXEMU_DIAGNOSTICS=$PWD/build/anon-measure/regression.txt sh scripts/run-full-regression.sh` (via `ssh -tt`). Suites that check their own diagnostics files override the variable; failures caused only by the global variable are expected and don't matter in this run. Then `grep anon-exec-svc build/anon-measure/regression.txt`.
- [ ] **Step 2: Corpus.** `sh scripts/run-corpus.sh` (it already sets diagnostics per app); `grep -r anon-exec-svc build/corpus/`.
- [ ] **Step 3: Decide.** Expected hits: `runtime-exec-syscalls` and `jit-patch-hazard` only (identify each record by its `pid`/comm field). **Any other program with a hit: stop and report it to the user before Task 4.**
- [ ] **Step 4: Record** in `docs/FINDINGS.md` a 2026-11-2x entry "Phase 4 part 1: what anonymous patching was used for": the hits, which programs, and the conclusion. Include the 2026-11-21 raw `svc #0` probe from the spec ("What the device probe found"). Commit "Record what anonymous executable patching was used for".

### Task 3: LuaJIT JIT smoke

**Files:**
- Create: `scripts/jit-apps/luajit.sh` (app definition in the corpus format: `corpus_packages="luajit"`, `stage_startup`, `stage_basic`, `stage_concurrent`, `stage_shutdown`)
- Create: `docs/corpus/luajit-apks.sha256` (via `CORPUS_APPS_DIR=scripts/jit-apps sh scripts/corpus/fetch-apks.sh luajit` on the device)
- Create: `scripts/run-jit-smoke.sh`
- Modify: `scripts/run-full-regression.sh` (add `run_suite run-jit-smoke` after `run-libuv-smoke`), `README.md` and `docs/development.md` suite count 16 → 17

**Interfaces:**
- Produces: `scripts/run-jit-smoke.sh` prints `luajit install=… startup=… basic=… concurrent=… shutdown=… compatible=…` and ends with `linuxemu_jit_failures=N`; exit 0 only when N is 0. It runs `CORPUS_APPS_DIR="$project_dir/scripts/jit-apps" CORPUS_OUT="$project_dir/build/jit-smoke" sh scripts/run-corpus.sh luajit` and fails unless the app is compatible. `scripts/corpus/apps/` is untouched, so the corpus stays at 11 apps.

- [ ] **Step 1: Pin LuaJIT** (`luajit` 2.1_p20251030-r0) with `fetch-apks.sh` as above; commit the manifest only.
- [ ] **Step 2: Write `scripts/jit-apps/luajit.sh`.** `stage_startup`: `luajit -v` prints `LuaJIT 2.1`. `stage_basic`: one Lua workload (numeric loop, table fill and sum, string building; at least 10^6 iterations), run three ways: `luajit -joff`, `luajit` (JIT on) and `luajit -jv` (stderr must contain `[TRACE`); the JIT-on and JIT-off stdout must be identical, and `jit.status()` must print `true` with the JIT on. `stage_concurrent`: four JIT-on workloads in parallel, each output equal to the JIT-off output. `stage_shutdown`: nothing left running (`corpus_kill_guests` leaves no pid).
- [ ] **Step 3: Write `scripts/run-jit-smoke.sh`** per the Interfaces block; add it to `run-full-regression.sh`.
- [ ] **Step 4: Run it on the Task 1 core** (anonymous memory still patched) and record the result: pass, or the failure and its stage. Either is acceptable now; it is the "before" measurement.
- [ ] **Step 5: Commit** "Add a LuaJIT JIT smoke to the full regression".

### Task 4: Stop patching anonymous memory

**Files:**
- Modify: `src/linux_syscall.c` (`linux_mprotect_runtime`; switch state and setter), `src/linuxemu.h`, `src/linuxemu.c` (read the switch at start-up), `src/guest_process.c` (pass it across `execve` like `LINUXEMU_DIAGNOSTICS`, lines ~261–268)
- Create: `guest-tests/runtime-exec-plain.S`, `guest-tests/runtime-exec-mixed.S`, `guest-tests/exec-argv1.S`
- Modify: `guest-tests/jit-patch-hazard.S` (header comment: now a regression test), `scripts/build-native-probes.sh` (guest list), `scripts/run-linuxemu-smoke.sh`

**Interfaces:**
- Consumes: `guest_memory_runtime_anonymous_at`, `anonymous_patch_candidate` (Task 1).
- Produces: `void linux_syscall_set_patch_anonymous(int enabled);` called from `src/linuxemu.c` with `getenv("LINUXEMU_PATCH_ANONYMOUS")` equal to `"1"`.

- [ ] **Step 1: Write the failing tests.**
  - `runtime-exec-plain.S`: mmap an anonymous RW page, store `mov r0, #42` (`0xe3a0002a`) and `bx lr` (`0xe12fff1e`) at offset 0, and the words `0xef000000`, `0xee1d0f70` at offset 64; `mprotect` RX; `blx` the page and require r0 = 42 (else exit 63); require both data words unchanged (else exit 65); exit 0.
  - `runtime-exec-mixed.S`: mmap two anonymous RW pages; store `0xef000000` at offset 0 of the second page; open the test's own binary (path from `argv[0]`; `run_guest` passes the host path) and mmap its first page `MAP_PRIVATE|MAP_FIXED`, `PROT_READ|PROT_WRITE`, over the first page (a writable private file mapping, so Linuxemu copies it and registers it file-backed). Count the words equal to `0xef000000` in the file page (must be > 0: the binary's own `svc` instructions, else exit 68). `mprotect` both pages RX in one call. Require the anonymous word still `0xef000000` (else exit 66) and no `0xef000000` word left in the file page (else exit 67); exit 0.
  - Smoke changes: `run_guest jit-patch-hazard 0`, `run_guest runtime-exec-plain 0`, `run_guest runtime-exec-mixed 0`; `runtime-exec-syscalls` runs as `LINUXEMU_PATCH_ANONYMOUS=1 run_guest runtime-exec-syscalls 0`; add `patch-anon-exec`: `LINUXEMU_PATCH_ANONYMOUS=1` with a guest that `execve`s `runtime-exec-syscalls` (new `guest-tests/exec-argv1.S`: `execve(argv[1], &argv[1], envp)`, exit 70 if it returns; add it to the guest list) must exit 0.
- [ ] **Step 2: Build and run the smoke on the Task 1 core.** Expected: `jit-patch-hazard` exit 20, `runtime-exec-plain` exit 65, `runtime-exec-mixed` exit 66; the switch cases pass (the switch doesn't exist yet, and anonymous memory is still patched).
- [ ] **Step 3: Implement.** Anonymous runs turned executable: no `arm_patch_range`; `mprotect` to the final protection, then `msync(MS_INVALIDATE_ICACHE)` over the run. File-backed runs: today's path (RW, `arm_patch_range`, icache sync, final protection). With the switch on, anonymous runs take the file-backed path. The Task 1 check runs for anonymous runs either way. On any failure, roll back patches made in this call (`arm_patch_rollback(patches_before)`) as today.
- [ ] **Step 4: Rebuild; run `run-linuxemu-smoke.sh`.** Expected: all cases pass, `linuxemu_smoke_failures=0`.
- [ ] **Step 5: Diagnostics without the switch.** Run `runtime-exec-syscalls` *without* the switch under `LINUXEMU_DIAGNOSTICS`: the report must contain `detail=anon-exec-svc` (the guest itself may now fail; record its exit status — it must not hang; use the 10 s timeout). Add this as smoke case `diag-anon-svc-unpatched` checking only the record.
- [ ] **Step 6: Run `run-jit-smoke.sh`.** Expected: `linuxemu_jit_failures=0`.
- [ ] **Step 7: Commit** "Stop patching anonymous memory made executable".

### Task 5: Flip cost and Node

**Files:**
- Create: `guest-tests/jit-flip-bench.c`, fixture `guest-tests/fixtures/jit-flip-bench-armhf` (built with `ALPINE_ROOT=build/fixture-rootfs sh scripts/build-guest-fixture.sh jit-flip-bench` on the device)
- Modify: `docs/FINDINGS.md`

- [ ] **Step 1: Write `jit-flip-bench.c`**: map 256 KiB anonymous RW, fill with a repeating word pattern that contains no match, then N=1000 RW→RX→RW flips with `mprotect`; print `flips=N usec=T` using `clock_gettime(CLOCK_MONOTONIC)`. Not a regression test.
- [ ] **Step 2: Run it** on the Task 1 core (`build/linuxemu-phase4a-before`, keep a copy) and the Task 4 core, three runs each, alternating.
- [ ] **Step 3: Node.** With the corpus `node` app's pinned closure installed (`sh scripts/run-corpus.sh node` leaves the measurement in `build/corpus/node/`), plus by hand `node -e 'console.log(1+1)'` and `node --jitless -e 'console.log(1+1)'` with diagnostics, on both cores. Record the fatal pc/address and the unsupported calls.
- [ ] **Step 4: Record** both in FINDINGS ("Phase 4 part 1: flip cost and Node"). Commit "Measure JIT flip cost and Node on Phase 4 part 1".

### Task 6: Docs and gate

**Files:**
- Modify: `docs/compatibility.md` (replace "Memory that a JIT flips to executable is scanned blindly…" with: anonymous memory made executable is not patched; generated code that makes Linux system calls needs `LINUXEMU_PATCH_ANONYMOUS=1`; with diagnostics on, such memory is reported as `anon-exec-svc`), `docs/development.md` (the switch, the JIT smoke, the closed JIT hazard item), `docs/FINDINGS.md` (hazard entry: hand-off closed by this part), `docs/capability-roadmap.md` ("Phase 4 part 1 status" before "## Phase 5", same shape as the Phase 3 part statuses), `docs/corpus-results.md` (part 1 run)

- [ ] **Step 1: Gate runs on the final core** (battery first): `run-full-regression.sh` → `linuxemu_full_regression_failures=0`; `run-corpus.sh` → no app compatible before is lost (10 of 11; any app's new `anon-exec-svc` records listed, not counted as a regression).
- [ ] **Step 2: Write the docs** above with the gate numbers and the core's sha256.
- [ ] **Step 3: Commit** "Record the Phase 4 part 1 gate". Merging to master and pushing need the user's OK.
