# Phase 4 part 2: Thumb runtime patching — design

Approved by the user 2026-11-23 (short design; a small project).

## Intent

Android's libraries are mostly Thumb-2, and bionic's linker maps every
library itself at run time. Linuxemu already splits the main program and its
interpreter into ARM, Thumb and data runs by mapping symbol (`$a`/`$t`/`$d`)
and patches each correctly. Libraries mapped at run time still get the
ARM-only blind scan: their Thumb `svc`/TPIDRURO reads are missed, and data
words that look like ARM `svc #0` are rewritten. This part closes that gap
and the two mode-transition gaps (signal handlers, entry points). Alpine's
armhf userland is ARM, so today's users see no change.

## Goals

1. **Runtime file mappings use mapping symbols.** `arm_patch_elf_mapping`
   reads the file's `.symtab` and splits each executable section the same
   way the static loader does: ARM runs to `arm_patch_range`, Thumb runs to
   `arm_patch_thumb_range`, data runs skipped. A file with no `.symtab` (or
   no mapping symbols) keeps today's ARM-only behaviour.
2. **`mprotect` stops re-scanning file pages.** File-backed pages were
   patched when they were mapped, with the right modes; making them
   executable later patches nothing. (Same reasoning as part 1's JIT rule:
   code written into memory at run time makes no system calls.)
   `LINUXEMU_PATCH_ANONYMOUS=1` keeps its meaning for anonymous memory.
3. **Thumb signal handlers and restorers.** A handler or `sa_restorer`
   address with bit 0 set runs in Thumb state: bit 0 cleared from the PC,
   CPSR T bit set (and cleared for an ARM handler).
4. **Thumb entry points are accepted** for the main program and its
   interpreter: bit 0 of `e_entry` selects Thumb start state.

## Non-goals

- Stripped binaries (no `.symtab`): executable-segment decoding is part 3.
- The Android gate items (AOSP `libc.so`/`libm.so`/`libutils.so`, NDK raw
  `syscall()`) and the modified-bionic decision: later parts.
- Patching anonymous memory (part 1 decided: never, by default).

## Anti-goals

- Any regression in ARM-only guests (all of Alpine): the full regression
  and corpus must stay as they are.
- Rewriting data: a `$d` run in a runtime-mapped library must stay intact.

## Approaches

- **Mapping symbols at mmap time (chosen).** Reuses the static loader's
  split and the existing Thumb decoder; works for every library built with
  symbols (NDK and Alpine debug builds, test fixtures).
- **Decode executable segments without symbols.** Needed for stripped
  AOSP libraries, but it is the larger, heuristic job of part 3.
- **Keep re-scanning on `mprotect`, Thumb-aware.** Would need the code
  state remembered per page; pointless, since the pages were already
  patched at mmap.

## Tests (builder's choices within this)

Guest fixtures built on the device with Alpine's gcc
(`-march=armv7-a -mthumb`), no Android NDK needed:

- a mixed ARM/Thumb shared library loaded with `dlopen`: its Thumb `svc`
  and Thumb TPIDRURO read work, and a `$d` word equal to `0xef000000`
  stays intact;
- a Thumb signal handler (and one with a Thumb restorer) runs and returns;
- a Thumb-entry static binary starts;
- a file page mapped read-write then made executable is not modified by
  the `mprotect`.

Gate: full device regression 0 failures, corpus 10/11 (same apps).

## Left to the builder

Where the shared mapping-symbol code lives (the loader's helpers move to
`arm_patch.c` or `thumb_decode.c`), and the exact fixtures.
