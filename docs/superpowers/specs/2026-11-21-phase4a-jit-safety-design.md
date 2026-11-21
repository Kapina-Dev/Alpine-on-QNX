# Phase 4, part 1: JIT safety — design

Date: 2026-11-21. Roadmap: `docs/capability-roadmap.md`, "Phase 4: Thumb and
stripped-binary support". Phase 3 is merged and released (v0.1.7, `81ee210`).
**Order chosen by the user: JIT safety first; Thumb runtime patching and
stripped binaries are later parts of Phase 4.** **Approach decided by the
user (2026-11-17, Android 7.1 notes): JIT code contains no system calls, so
nothing in it needs patching, and the writable/executable flips just have to
be cheap.**

## Intent

When a guest makes anonymous memory executable with `mprotect`, Linuxemu
scans the whole range for words that look like `svc #0` (`0xef000000`) or a
TPIDRURO read (`0xee1d0f70` under mask `0xffff0fff`) and rewrites them into
traps. It can't tell code from data, so a JIT's data words with those values
are corrupted. Phase 2 measured it (`guest-tests/jit-patch-hazard.S`,
`docs/FINDINGS.md` 2026-11-18, "runtime-mapping patch hazard"). V8 keeps
constant pools inside its code pages and reads them while running, so this
can corrupt real programs, and every flip pays for a full scan.

Part 1 makes JIT memory safe: anonymous memory that becomes executable is no
longer patched. It's for anyone running a JIT under Linuxemu: Node (V8),
LuaJIT, and later Android's ART JIT.

## What the device probe found (2026-11-21, native QNX, ordinary user)

A throwaway probe (`build/svcp/` on the Passport, not in the repo) ran a Linux
`svc #0` in ARM and Thumb state in a forked child, with `ip` set to an
invalid QNX kernel-call number (`0x7fff`):

- QNX's own kernel calls are `svc 0x51` with the call number in `ip`; they
  return to `svc + 4` on success and `svc + 8` on error (libc stubs:
  `svc 0x51; pop {…, pc}; pop {…, lr}; b __errno_path`).
- The Linux `svc #0` came back at the error return in both states (SIGILL
  only afterwards, from executing past the stub). **QNX ignores the `svc`
  immediate**: an unpatched Linux `svc` is never trapped, it silently runs
  whatever QNX kernel call `ip` happens to name.

So lazy trapping (catching an unpatched `svc` when it runs) is impossible,
and system calls must still be patched ahead of time wherever they can
occur. The design relies on JIT memory not containing any.

## Goals

- Anonymous memory flipped to executable is never modified by Linuxemu.
  `jit-patch-hazard` reads its data back intact.
- Flips are cheap: no rewrite pass over the range.
- A real JIT runs with its JIT on, on the Passport, and gives the same
  results as with the JIT off.
- The rare program that generates real system calls into anonymous memory
  is visible, not silent, and has a way to run.
- Node's start-up crash is checked against this change, and the result is
  recorded (it may or may not be this hazard).

## Non-goals

- Thumb patching of libraries mapped at run time, Thumb signal handlers and
  entry points, stripped-binary decoding, the Android gate items, and the
  modified-bionic decision: later Phase 4 parts.
- Literal pools inside ELF `.text` sections are still scanned as code today
  (except where `$d` mapping symbols mark them, in static binaries with a
  symbol table). That is the stripped-binary/decoding part, not this one.
- Making Node work. Its start-up also needs `madvise` and `mremap` of
  executable memory (Phase 6); part 1 doesn't promise Node.
- `mremap` of executable memory stays refused.

## Anti-goals

- Any change that makes a generated `svc` run a random QNX kernel call
  *without* a trace when diagnostics are on.
- Slowing down the flip path (the point is to make it cheaper).
- Changing how file-backed mappings are patched.

## Constraints

- An unpatched `svc` runs a QNX kernel call (probe above), so "don't patch"
  is only safe for memory that has no Linux system calls in it.
- `guest-tests/runtime-exec-syscalls.S` (in `run-linuxemu-smoke` since
  2026-09-23) writes `mov r7,#20; svc #0; bx lr` into anonymous memory,
  flips it RX and expects `getpid`. That contract changes (see Design).
- Linux doesn't flush the instruction cache on `mprotect`; JITs call
  `cacheflush` themselves (V8, and LuaJIT through `__clear_cache`).
- Device facts: `ssh -o HostName=<ip> bb10`, never `-tt` when piping;
  check the battery with `sensors` before long runs.

## Approaches considered

1. **Stop patching anonymous executable memory (chosen).** JIT code calls
   into libc for system calls and doesn't read TPIDRURO directly, so nothing
   in it needs patching. Sound for JITs, cheapest flips. Cost: generated code
   with a real `svc` in anonymous memory no longer works by default.
2. **Patch, but undo the patches whenever the page turns writable and redo
   them on the flip back.** The JIT then reads its own code back correctly
   while writing, but its constant pools are still corrupted while the code
   runs (V8 reads them from executable pages). Half a fix, and still a scan
   per flip.
3. **Guess code from data**, e.g. patch an `svc` word only after a plausible
   `mov r7, #N`. Fewer false hits, but still guessing, more complex, and
   silent when it guesses wrong either way. ARM instructions are fixed
   4-byte words, so a data word equal to `svc #0` is indistinguishable from
   one.

## Design

### The mprotect path

`linux_mprotect_runtime` (`src/linux_syscall.c`) today, for any runtime
mapping turned executable: `mprotect` to RW, `arm_patch_range` over the whole
range, `msync(MS_INVALIDATE_ICACHE)`, `mprotect` to the final protection.

New rule, decided per page run by the runtime mapping registry
(`src/guest_memory.c`, `struct runtime_mapping`, `anonymous` flag):

- **Anonymous runs** (private anonymous, not file-backed) turned executable:
  no `arm_patch_range`. `mprotect` straight to the final protection, then
  `msync(MS_INVALIDATE_ICACHE)` over the range, kept as a safety net because
  Linuxemu did it before and it's cheap next to a scan (my call; drop it if
  the flip timing shows it matters).
- **File-backed runs**: unchanged (patched as today).
- A range that mixes anonymous and file-backed runs: each run is handled by
  its own rule.

Memory Linuxemu itself mapped with host `MAP_ANON` for a guest *file*
mapping (the `pread` copy paths in `linux_mmap2`) is registered as
file-backed, so it keeps its patching. An anonymous `mmap` that is
executable from the start is already not scanned; that stays.

Patch sites already recorded in an anonymous range (only possible with the
switch below) are left as they are.

### The diagnostic check

When `LINUXEMU_DIAGNOSTICS` is on (and only then, so normal runs pay
nothing), an anonymous run turned executable is read, not written, for the
same patterns `arm_patch_range` matches (ARM `svc #0` word and ARM TPIDRURO
read). If any is found, one diagnostics record is made per flip:
`mprotect` with detail `anon-exec-svc`, error 0, and the first matching
address in the arguments. A JIT literal pool can match too, so this is a
hint, not an error; the per-process report already caps entries
(`LINUX_DIAG_MAX_ENTRIES`). Thumb patterns are not checked (`0xdf00` is too
common in data to be a useful hint).

### Measure before changing the default

The diagnostic check is built first, while anonymous memory is still
patched. Then the full regression and the corpus run once with
`LINUXEMU_DIAGNOSTICS` on, and every `anon-exec-svc` record is listed.
Expected hits: only `runtime-exec-syscalls` and `jit-patch-hazard`. Any other
program with a hit is one this change would affect: stop and bring it to the
user before the default changes. The result goes in FINDINGS. (Asked for by
the user, 2026-11-21: "what does this break from Phase 3?")

### The switch

`LINUXEMU_PATCH_ANONYMOUS=1` restores the old behaviour (patch anonymous
memory on the flip) for a program that really generates system calls into
anonymous memory. It is read once at start-up and must survive `fork` and
`execve` the same way `LINUXEMU_DIAGNOSTICS` does (the builder checks how
`src/guest_process.c` builds the host environment for `execve`). Name is my call.

### runtime-exec-syscalls

The test keeps its code (generated `svc` in anonymous memory) and runs with
`LINUXEMU_PATCH_ANONYMOUS=1` in `run-linuxemu-smoke`, proving the switch. A
new default-mode test covers the new contract: generated code without a
`svc` (`mov r0, #42; bx lr`) runs after a flip and returns 42, and a data
page holding the two patterns reads back intact after its flip.

### Docs

`docs/compatibility.md` (the "scanned blindly" limit becomes "not patched;
a generated `svc` needs `LINUXEMU_PATCH_ANONYMOUS=1`"), `docs/FINDINGS.md`
(the svc probe; the hazard entry's hand-off closed), `docs/development.md`
(the switch), and the roadmap's Phase 4 part 1 status.

## Tests

- `jit-patch-hazard` becomes a pass/fail regression test (exit 0 = data
  intact and the 5000-flip loop completes), in `run-linuxemu-smoke`. Run it
  first on the current core and see it fail (exit 20).
- The new default-mode test (above), and `runtime-exec-syscalls` with the
  switch. The default-mode test fails on the current core for the data
  check.
- A diagnostics test in the style of the existing `diagnostics-*` guests:
  with `LINUXEMU_DIAGNOSTICS` set, a flipped anonymous page holding an `svc`
  word produces one `anon-exec-svc` record; without diagnostics, none.
- Flip cost: a loop of RW→RX flips over a 256 KiB anonymous region, timed on
  the current core and the new one; recorded, not gated (the new one should
  be faster).
- **LuaJIT on the phone** (`luajit` 2.1_p20251030-r0 is in Alpine armhf):
  install it pinned by sha256 like the corpus apps, run a small CPU workload
  with the JIT on (check `jit.status()` is true and traces were compiled)
  and with `-joff`, and compare the output. A JIT smoke runner joins the
  full regression (my call; the corpus stays at its pinned 11 apps).
- **Node check** (measurement, not gate): `node -e` on the current and new
  cores, default and `--jitless`, with diagnostics; record what changed in
  FINDINGS.

## Gate for part 1

- Full device regression: 0 failures, including the new tests.
- `jit-patch-hazard` passes; LuaJIT JIT-on output matches JIT-off.
- Corpus: no app compatible before is lost (10 of 11).
- Node's result recorded either way.

## Decided by the user

- Phase 4 order: JIT safety first.
- Approach: JIT memory isn't patched (2026-11-17: "JIT code contains no
  syscalls, so nothing needs patching; the flips just have to be cheap").
- Measure what the change would affect (regression and corpus with the
  diagnostic check) before the default changes.

## My calls (correct me)

- The diagnostic check exists, and only runs with `LINUXEMU_DIAGNOSTICS`.
- The `LINUXEMU_PATCH_ANONYMOUS=1` switch and its name.
- `runtime-exec-syscalls` moves to the switch; generated `svc` in anonymous
  memory is no longer supported by default.
- The icache `msync` stays on the anonymous flip path.
- LuaJIT as the real JIT, in a new smoke runner in the full regression,
  not in the corpus.

## Left to the builder

- Where the per-run split lives (in `linux_mprotect_runtime` or a
  `guest_memory` helper that walks runs and reports anonymous/file-backed).
- The LuaJIT workload and how traces are confirmed.
- Exact names of the new tests and runner.
