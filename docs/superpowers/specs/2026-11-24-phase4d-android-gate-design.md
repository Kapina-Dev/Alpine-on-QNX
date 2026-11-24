# Phase 4 part 4: the Android gate

Status: design, 2026-11-24. The last part of roadmap Phase 4. Parts 1–3
(JIT safety, Thumb runtime patching, stripped-binary decoding) are done.

## Intent

Prove that Android's own code, run by Android's own dynamic linker, is
patched correctly by Linuxemu: every system call and TLS read found, no
data word changed, no file refused. This is the roadmap's Phase 4
"Android additions" gate. It also records the decision on whether Phase 9
needs a modified bionic.

The user chose (2026-11-24):

- **Gate option (a): stop at the pid abort.** 32-bit bionic aborts at
  start-up when the pid is above 65535 ("32 bit bionic libc only accepts
  pid <= 65535"), and QNX pids are huge. Pid virtualisation is Phase 6, and
  phases go in order. So the gate ends at that abort: the linker has
  mapped, decoded, relocated and patched every library, and libc's Thumb
  start-up code has run far enough to make the system call that reads the
  pid.
- **Approach 1:** fix the decoder's wrong-mode bug and extend its coverage
  first, measured on the laptop over the whole Android `/system/lib`, then
  run the gate on the device.
- **Android libraries from Google's API 23 armeabi-v7a emulator image,**
  plus NDK r27c. Neither is ever committed.

## Facts this design rests on (checked 2026-11-24, laptop)

- **Image:** `armeabi-v7a-23_r06.zip`, SHA-1
  `7cf2ad756e54a3acfd81064b63cb0cb9dff2798d` (Google's repository
  manifest). It's **Android 6.0** (`ro.build.fingerprint`
  `Android/sdk_phone_armv7/generic:6.0/MASTER/3079352:userdebug/test-keys`),
  not 6.0.1. Google published no 6.0.1 ARMv7 emulator image. Both are
  API 23; bionic barely changed between them. The roadmap's "6.0.1" is
  met by API level, and this difference is recorded.
- **NDK:** `android-ndk-r27c-linux.zip`, SHA-1
  `090e8083a715fdb1a3e402d0763c388abb03fb4e`. Unpacked on the laptop at
  `build/android/android-ndk-r27c`. `/system/bin` and `/system/lib` are
  extracted with `debugfs rdump` to `build/android/system` (70 MB).
- **Not every library is stripped.** `libc.so` (1764 `$t`, 251 `$a`, 2863
  `$d`) and `libart.so` keep `.symtab` with mapping symbols. They take
  part 2's mapping-symbol path, not decoding. The other 216 files in
  `/system/lib` have no mapping symbols.
- **`/system/bin/linker` keeps a `.symtab` with 573 `STT_FUNC` symbols
  but no mapping symbols.** `code_map_from_elf` reads files "without
  their .symtab", so it ignores them.
- **Part 3's decoder over `/system/lib` today:** 7 of 218 refused
  (`libart`, `libc`, `libEGL`, `libjavacore`, `libLLVM`,
  `libOpenglSystemCommon`, `libskia`). `libc` and `libart` don't matter
  at run time because they have mapping symbols. `/system/bin/linker` is
  refused (first site `0xf1ba`), so **the gate can't pass today.**
  4 files show no Thumb evidence and take the ARM-only scan. One is
  `libm.so`, whose 380 `.dynsym` functions are all ARM (correct).
  `libutils.so` decodes and isn't refused.
- **The gate program loads 10 libraries, not 3.** `libutils.so` pulls in
  `libdl`, `libbacktrace`, `libcutils`, `liblog`, `libc++`, `libbase` and
  `libunwind`. All of them decode without refusal today. The device
  rootfs needs all 10 plus the linker.
- `src/elf_loader.c` accepts `/system/bin/linker` as a `PT_INTERP` path
  (absolute, no `.`/`..`).
- **Wrong-mode bug.** `code-map-test libc.so libc.stripped.so` (stripped
  with `llvm-strip`):
  - refused at `0x174f0`;
  - 414 code bytes decoded in the wrong mode, 0 data bytes decoded as
    code, 28806 code bytes left undecoded.
  - The first wrong byte is at `0x607fc`: a linker-generated Thumb→ARM
    interworking stub, Thumb `bx pc; nop` (`4778 46c0`) followed by ARM
    `ldr ip, [pc]; add pc, ip, pc` and a data word. Mapping symbols:
    `$t` at the stub, `$a` 4 bytes on, `$d` at the word. The stubs follow
    an ARM function (`__udivdi3`) and are decoded as part of it.
- From part 3: GNU ld merges adjacent identical `.ARM.exidx` entries;
  `.dynsym` and `R_ARM_RELATIVE` function words carry the Thumb bit;
  exidx starts don't. Functions reached only through
  `ldr rX, =off; add rX, pc` are the known coverage gap (stripped Debian
  glibc is refused for it).
- From the bionic probe (`docs/FINDINGS.md`): static bionic start-up makes
  `set_tls`, `getpid`, `mmap2`, `personality` and `prctl` calls and then
  aborts on the pid check before `main()`. All of those are handled now.
  The abort is a self-sent `SIGABRT` (exit 134), which flushes
  diagnostics.

## Goals

1. **Fix the wrong-mode bug.** A Thumb→ARM stub (`bx pc` with `nop`, at
   a halfword-aligned address, reached by a Thumb branch, followed by ARM
   code) is decoded as a 4-byte Thumb function followed by an ARM one,
   whatever exidx range it falls inside. Stripped `libc.so` against its
   original: **0 bytes in the wrong mode.**
2. **Use `.symtab` function symbols when there are no mapping symbols.**
   An `STT_FUNC` symbol in `.symtab` is function evidence with its mode,
   like a `.dynsym` one. It's trusted the same way: never as data, and it
   starts a function. This covers `/system/bin/linker`.
3. **Follow PC-relative address formation.** Treat `ldr rX, =off` followed
   by `add rX, pc` (and `movw`/`movt` followed by `add rX, pc`, both
   modes, with the PC offset of the `add`'s mode) as evidence. The result,
   with its Thumb bit, names a function start of that mode when it lands
   in an executable section and outside known data. It's **weak**
   evidence (part 3's sense): it may name data, so it never cuts another
   function and decodes only bytes nothing else claims.
4. **A laptop survey** (`code-map-survey`, a host tool beside
   `code-map-test`) that runs the decoder over every ELF file in a
   directory tree and prints one line per file: mapping symbols present
   or not, Thumb evidence, refused (with address), unsettled, decoded
   bytes. For files that have mapping symbols, it strips a copy and runs
   the existing pair comparison. The design's numbers come from it.
5. **Gate libraries pass on the laptop:** `/system/bin/linker`,
   `libc.so` (stripped copy), `libm.so` and `libutils.so` are not
   refused. The `libc.so` and `libart.so` pairs show 0 wrong-mode and 0
   data-as-code bytes. The survey's refusal count over `/system/lib` and
   `/system/bin` is recorded before and after.
6. **Device gate, dynamic.** A Thumb NDK program whose `PT_INTERP` is
   `/system/bin/linker`, with `DT_NEEDED` `libc.so`, `libm.so` and
   `libutils.so`, runs under Linuxemu with an Android guest root:
   - no `ENOEXEC`/`undecodable-code` record for any file;
   - the linker maps and relocates all three libraries and their 7
     dependencies (bionic's linker loads every library before any
     constructor runs);
   - libc's start-up runs and stops with bionic's pid message and exit
     134, the expected end;
   - the number of patch sites Linuxemu reports per file equals the
     laptop's count for the same file.
7. **Device gate, raw `syscall()`.** A static, stripped, Thumb NDK
   program with its own entry point (`-nostartfiles`, so bionic's
   start-up and its pid check never run) calls the NDK `libc.a`'s
   `syscall()` for `write`, `getpid` and `exit_group`, and makes one
   inline Thumb `svc #0`. It passes when the output and exit status
   match. The decoder must decode it (it's stripped), and its system
   calls must be intercepted.
8. **The modified-bionic decision** is written into `docs/FINDINGS.md`
   from the gate's evidence. The user makes the call when the gate is
   done. The design's input: with goals 1–7 met, patching covers bionic,
   and a modified bionic would only change `libc.so`'s syscall stubs,
   which already work through mapping symbols.

## Non-goals

- Pid virtualisation, or anything that lets `main()` run in a dynamic
  bionic program (Phase 6).
- Zero refusals over all of `/system/lib`. The other refused libraries
  (`libLLVM`, `libskia`, `libEGL`, …) are measured and listed; they are
  fixed only if goals 1–3 fix them.
- ART, `.oat` files, `app_process`, Zygote.
- Matching 6.0.1 exactly.
- Committing any Google or NDK binary, or a library extracted from one.
- Files without section headers (`sstrip`): unchanged.
- General indirect-branch or data-flow analysis beyond goal 3's two-
  instruction patterns.

## Anti-goals

- Patching bytes the decoder couldn't classify. Refusal stays the answer
  when instruction boundaries are unknown.
- Guessing a function's mode from its bytes. The stub rule (goal 1) is a
  fixed linker-generated sequence, not a guess.
- Any change to Alpine behaviour: the full regression and the corpus must
  not move.

## Constraints

- Phase order: no Phase 5/6 work pulled forward.
- Downloads come from scripts that check the SHA-1s above:
  `scripts/fetch-android-sysimg.sh`, which downloads, verifies, extracts
  `/system/bin`, `/system/lib` and `build.prop` with `debugfs`, and
  deletes the zip and image, and `scripts/fetch-android-ndk.sh`.
  Outputs go under `build/android/` (ignored). Like
  `fetch-sqlite-amalgamation.sh`, these run on the laptop.
- Laptop disk is tight (about 5 GB free after setup). Scripts delete
  archives as soon as they are unpacked.
- The device rootfs for the gate (`/system/bin/linker`, the needed
  `/system/lib` files, the test programs) is built on the laptop and
  uploaded to the device dev tree under `build/`. It's never committed.
  It is separate from the Alpine test rootfs.
- Check the device's battery (`sensors`) before device runs.
- Commits follow the privacy rules (no session trailers; audit before any
  push).

## Approaches

1. **Extend the decoder and fix the wrong-mode bug, measure on the laptop,
   then run the device gate (chosen).** It fixes a real correctness bug
   and gives coverage numbers over a real Android userland before any
   device time.
2. **Device gate first, with a temporary bypass for the linker.** Faster
   to a device run, but it leaves the wrong-mode bug in place and works
   against "never patch what can't be decoded".
3. **Decide on modified bionic first.** It only spares `libc.so`, which
   already works through mapping symbols. The linker and the other
   libraries still need decoding.

## How the gate is checked

- **Laptop:** `code-map-test` (existing checks plus new fixtures for the
  stub rule, `.symtab` functions and PC-relative formation), the
  `libc.so` and `libart.so` pairs, and the survey over
  `build/android/system`.
- **Device:** the dynamic test (goal 6) and the static syscall test
  (goal 7), run from a new script that sets `LINUXEMU_ROOT` to the
  Android rootfs and turns on `LINUXEMU_DIAGNOSTICS` and
  `LINUXEMU_VERBOSE`. It isn't added to the full regression: the Android
  files aren't in the repo. It is documented in `docs/development.md`.
- **No change elsewhere:** full device regression
  `linuxemu_full_regression_failures=0`, corpus 10 of 11 (same apps).

## Left to the builder

- The exact stub recognition (where the rule hooks into mode propagation;
  whether ARM→Thumb stubs need the mirror rule; the survey will show).
- How `.symtab` is read when it lacks mapping symbols: inside
  `code_map_from_elf` or passed in by `arm_patch.c`. Either way, the
  runtime's choice between mapping symbols and decoding stays as it is
  (any `$a`/`$t`/`$d` present → mapping-symbol path).
- How the patch-site count per file is reported on the device (a
  `LINUXEMU_VERBOSE` line per decoded mapping is enough) and how the
  laptop count is printed (`code-map-survey`).
- Whether Linuxemu's loader resolves `PT_INTERP` `/system/bin/linker`
  under the guest root as it does for musl's loader. If not, fixing that
  is in scope.
- Building the test programs: NDK r27c clang, `-mthumb`, API 23 for the
  dynamic one (linked against the image's `libutils.so` with `-L`), API
  29 crt for the static one if needed (as `build-bionic-probe.sh`
  explains).
- Any deferred part 3 minor that the gate trips over (the refusal record
  not naming the file is the likely one) may be fixed here.
