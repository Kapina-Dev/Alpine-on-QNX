# Phase 4 part 3: stripped-binary decoding — design

The user chose 2026-11-23: decoding only, proven on our own stripped Thumb
libraries; the Android gate (AOSP 6.0.1 libraries through bionic's linker)
is part 4.

## Intent

A file without a symbol table has no mapping symbols, so Linuxemu can't
tell its ARM code from its Thumb code from its data. Today it scans such a
file as ARM: Thumb system calls and TLS reads are missed (an unpatched `svc`
on QNX runs whatever QNX kernel call `ip` names, and can't be caught) and
data that looks like ARM `svc #0` is rewritten. Most Android libraries are
stripped and mostly Thumb-2, so this is the next blocker for the Android
track. The roadmap's stages 2 and 3: decode executable code of stripped
files safely, and refuse a file where safe instruction boundaries can't be
established.

## Facts this design rests on (checked 2026-11-23)

- Stripping removes `.symtab` but keeps section headers, `.dynsym`,
  `.rel.dyn` and `.ARM.exidx`. (The roadmap's "AOSP libraries ship without
  section headers" is corrected in FINDINGS by this part.)
- `.ARM.exidx` entries give function start addresses **without** the Thumb
  bit (checked on a `-funwind-tables -mthumb` library built with Alpine
  gcc). Android builds everything with `-funwind-tables`; Alpine's gcc does
  not by default, so Alpine libraries usually have no `.ARM.exidx`.
- `.dynsym` function values carry the Thumb bit (bit 0). So do function
  pointers stored in data (`R_ARM_RELATIVE` addends: `init_array`, vtables,
  callback tables) and `e_entry`.

## Goals

1. A file without mapping symbols that shows **no Thumb evidence** (no
   odd function address in `.dynsym`, `e_entry`, `DT_INIT`/`DT_FINI` or an
   `R_ARM_RELATIVE` word pointing into code) is patched exactly as today
   (ARM scan). All of Alpine stays on this path.
2. A file with Thumb evidence is **decoded**:
   - function starts come from `.ARM.exidx`; a function's extent runs to
     the next start (or the section end);
   - each function's mode comes from evidence (above) and is propagated
     through direct calls and branches (`bl`/`blx`/`b` targets) of decoded
     functions, to a fixpoint;
   - a function is decoded instruction by instruction in its mode; words a
     PC-relative load reads (literal pools: `ldr`, `ldrd`, `vldr` literal
     forms) and inline `tbb`/`tbh` tables (bounded by the preceding `cmp`)
     are data;
   - the result is a synthesized `$a`/`$t`/`$d` list fed to the existing
     `arm_patch_code_runs`, so patching itself doesn't change.
3. **Refuse** (stage 3): if any byte range left undecoded (no function
   covers it, its mode is unknown, or decoding stopped at an unbounded
   table) holds a pattern that would be a patch site in either mode, the
   file is refused: `ENOEXEC` from the load or `mmap`, a
   `LINUXEMU_DIAGNOSTICS` record (`detail=undecodable-code`, the address),
   and a `LINUXEMU_VERBOSE` line. A range with no such pattern is left
   alone: nothing in it needs patching.
4. Both the program loader and runtime mappings use it (it sits behind
   `arm_mapping_symbols_load`).

## Non-goals

- The Android gate (AOSP libraries through bionic's linker), the
  modified-bionic decision: part 4.
- Files without section headers (`sstrip`): unchanged (the loader refuses
  them; a runtime mapping of one is not patched).
- Heuristic guessing of a function's mode from its bytes.
- Indirect-branch analysis beyond `tbb`/`tbh`.

## Anti-goals

- Any change for files with mapping symbols or without Thumb evidence (the
  whole Alpine userland): full regression and corpus unchanged.
- A missed patch site in a file that loads: a file either decodes fully
  where patterns exist, or is refused.
- Rewriting a literal-pool or table word.

## Approaches

- **Unwind-table functions + evidence-propagated modes, refuse when
  unsure (chosen).** Exact where the evidence reaches; Android's
  `-funwind-tables` makes coverage near-complete for its libraries.
- **Heuristic mode detection (score both decodings).** Covers files
  without evidence, but a wrong guess is a missed `svc` or a corrupted
  word, silently.
- **Whole-segment disassembly from entry points only (no unwind table).**
  Can't find functions reached only indirectly; leaves large gaps.

## Tests

- Host unit tests for the instruction facts and the map builder
  (hand-encoded Thumb and ARM: literal loads, `tbb`/`tbh` with and without
  a bound, calls, unknown-mode ranges with and without patterns).
- A generated mixed ARM/Thumb library built on the device with
  `-funwind-tables`: functions with inline `svc` and TLS reads, `switch`
  tables, literal pools holding `0xef000000`/`0xdf00` constants, static
  functions reached only by calls and by function pointers. Kept unstripped
  (ground truth) and stripped. A guest program `dlopen`s both copies and
  compares their mapped code byte for byte (same patches), then calls the
  stripped copy's functions.
- SQLite's amalgamation built `-mthumb -funwind-tables` as a real-world
  library, unstripped vs stripped, same comparison.
- A stripped Thumb static program runs (program-loader path).
- A stripped Thumb library with an unknown-mode function holding an `svc`
  is refused, with the diagnostic.
- Gate: full device regression 0 failures, corpus 10/11 (same apps).

## Left to the builder

The exact module split, the `cmp` look-back window for `tbb` bounds, and
the fixture generator.
