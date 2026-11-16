# Phase 0A: Structured Syscall Diagnostics Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** When `LINUXEMU_DIAGNOSTICS=<file>` is set, every guest process appends a
report of the unsupported syscalls and unsupported behaviour it hit. Each report
gives the number and name, the executable, a safe argument summary, the count,
and the Linux error returned.

**Architecture:**
- A pure module, `src/linux_diagnostics.c`, aggregates records in a fixed
  table and formats them without stdio or malloc. That makes it host-testable
  and safe to flush from the fatal-signal path.
- The runtime glue records at the dispatcher's ENOSYS default and at the
  explicit unsupported-behaviour sites.
- It flushes on every process exit path (`exit_group`, last-thread `_exit`,
  `execve`, fatal signal) and resets after `fork`.

**Tech Stack:**
- C99 (gnu99).
- The QNX 8 SDK gcc on the device, with host `cc` for unit tests.
- ARM assembly guest tests, using SDK `as`/`ld` and `guest-tests/minimal-arm.ld`.

**Spec:** `docs/capability-roadmap.md`, "Phase 0: measurable compatibility",
first paragraph and bullet list.

This plan covers only the diagnostics. The application corpus (Phase 0B) and the
`android-probe` corpus with `docs/android-syscall-gap.md` (Phase 0C) get their
own plans. Both consume this output.

## Global Constraints

- Report fields, from the spec:
  - "unsupported syscall number and name";
  - "calling executable";
  - "a safe argument summary";
  - "occurrence count"; and
  - "returned Linux error".
- "Return correct Linux errors for unsupported behavior; do not use success
  stubs."
- "Translate every structure, flag set, and error namespace that crosses the
  Linux/QNX boundary."
- "Preserve the current release regression": `scripts/run-full-regression.sh`
  must stay at `linuxemu_full_regression_failures=0`.
- "Keep ordinary-user execution as the supported environment." Diagnostics
  write only where the user points `LINUXEMU_DIAGNOSTICS`.
- "Retain a test for every compatibility fix."
- With `LINUXEMU_DIAGNOSTICS` unset, behaviour and output are unchanged.

## Report format (decided here; Phase 0B/0C parse it)

There is one line per distinct `(nr, error, detail)` per process, written in a
single `write()` with `O_APPEND`:

```
linuxemu-unsupported exe=<exe> pid=<pid> nr=<decimal> name=<name> error=<ENAME> count=<n> detail=<detail> args=0x%08x,0x%08x,0x%08x,0x%08x,0x%08x,0x%08x
```

- `exe` is the guest path, percent-encoded: bytes `<= 0x20`, `%` and
  `>= 0x7f` become `%XX` in uppercase hex.
- `name` comes from the generated table, or is `unknown`.
- `error` is the Linux errno name, or `E<number>` when the name isn't in the
  table.
- `detail` is a static identifier with no spaces (`-` for the plain ENOSYS
  default).
- `args` are r0–r5 of the **first** occurrence, as raw register values. Memory
  is never dereferenced, which is what makes them safe.
- When more than 64 distinct entries arrive, the extras are counted and one
  extra line is emitted:
  `linuxemu-unsupported-overflow exe=<exe> pid=<pid> dropped=<n>`.

## Review Focus

- **Diagnostics unset:** no file is created and the guest exit status is
  unchanged. Pinned in Task 2, Step 1 (`diag-off` check).
- **Unwritable report path** (for example `/nonexistent-dir/r.txt`): the guest
  still runs and returns its normal status. The failure is silent. Pinned in
  Task 2, Step 1 (`diag-unwritable` check).
- **fork and execve:** a fork child must not re-report the parent's counts,
  and an exec'd image must not re-report the pre-exec image's counts. Each
  reports only its own calls (a fork child under its own `pid=`; an exec'd
  image under the same pid, in a separate line). Pinned in Task 2's
  `diag-basic`.
- **More than 64 distinct unsupported calls:** nothing is lost silently. An
  overflow line carries the dropped count. Pinned in Task 1 (`overflow` test).
- **Executable paths with spaces or `%`:** the line stays one-token-per-field
  parseable. Pinned in Task 1 (`exe encoding` test).

---

### Task 1: Name tables and the aggregation/formatting module

**Files:**
- Create: `scripts/generate-syscall-names.sh`. It reads
  `$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/arm-linux-androideabi/asm/unistd-eabi.h`
  (`#define __NR_<name> (__NR_SYSCALL_BASE + <n>)`) and writes
  `src/linux_syscall_names.h`. Add the ARM private calls by hand in the
  script: `0xf0001 ARM_breakpoint`, `0xf0002 ARM_cacheflush`,
  `0xf0003 ARM_usr26`, `0xf0004 ARM_usr32`, `0xf0005 ARM_set_tls`,
  `0xf0006 ARM_get_tls`.
- Create: `src/linux_syscall_names.h`. This is generated and committed: a
  `static const struct { uint32_t number; const char *name; }
  linux_syscall_names[]` sorted by number, plus a header comment naming the
  NDK version (r27c) and the script.
- Create: `src/linux_diagnostics.h` and `src/linux_diagnostics.c`. The only
  dependencies are `<stdint.h>`, `<stddef.h>` and `<string.h>`.
- Test: `tests/diagnostics-test.c`, built and run on the host the same way as
  `tests/thumb-decode-test.c`:
  `cc -Isrc -Wall -Wextra -o <out> tests/diagnostics-test.c src/linux_diagnostics.c`.

**Interfaces:**
- Produces, in `src/linux_diagnostics.h`:
  - `const char *linux_syscall_name(uint32_t number);`: the name, or `"unknown"`.
  - `const char *linux_errno_name(int32_t linux_error);`: takes a positive
    Linux errno. It covers at least EPERM 1, ENOENT 2, ESRCH 3, EINTR 4, EIO 5,
    EBADF 9, EAGAIN 11, ENOMEM 12, EACCES 13, EFAULT 14, EEXIST 17, ENOTDIR 20,
    EINVAL 22, ENOTTY 25, ENOSPC 28, ESPIPE 29, ERANGE 34, ENOSYS 38,
    ENOTSUP/EOPNOTSUPP 95 (`"EOPNOTSUPP"`) and EAFNOSUPPORT 97. It returns
    `0` when the errno isn't in the table, and the formatter then prints
    `E<n>`.
  - `void linux_diag_record(uint32_t number, int32_t linux_error, const char *detail, const uint32_t args[6]);`
    `linux_error` is positive and `detail` is a static string, or `0`, which
    is printed as `-`. Matching entries add to `count`; the first `args` are
    kept.
  - `size_t linux_diag_format(char *buffer, size_t capacity, const char *executable, int32_t pid);`
    It writes every line, each ending in `\n`, and returns the bytes written.
    It stops at whole lines when `capacity` is short. It uses no stdio and no
    malloc.
  - `void linux_diag_reset(void);`: empties the table and the overflow count.
  - `#define LINUX_DIAG_MAX_ENTRIES 64`
- Concurrency: one spinlock (`__sync_lock_test_and_set` /
  `__sync_lock_release`) guards record and reset. `linux_diag_format` reads
  without the lock, because the fatal path must never block.

- [ ] **Step 1: Write the failing tests** in `tests/diagnostics-test.c`, using
  the same `check()` style as `tests/thumb-decode-test.c`:
  - `names`: `linux_syscall_name(136)` is `"personality"`,
    `linux_syscall_name(363)` is `"rt_tgsigqueueinfo"`,
    `linux_syscall_name(0xf0005)` is `"ARM_set_tls"` and
    `linux_syscall_name(999)` is `"unknown"`.
  - `errno names`: `linux_errno_name(38)` is `"ENOSYS"`,
    `linux_errno_name(95)` is `"EOPNOTSUPP"` and `linux_errno_name(200)` is `0`.
  - `aggregate`: after `linux_diag_reset()`, record nr 999 / ENOSYS / detail 0
    twice (args `{1,2,3,4,5,6}`, then `{9,9,9,9,9,9}`). Formatting with exe
    `"/bin/x"` and pid 42 must give exactly
    `linuxemu-unsupported exe=/bin/x pid=42 nr=999 name=unknown error=ENOSYS count=2 detail=- args=0x00000001,0x00000002,0x00000003,0x00000004,0x00000005,0x00000006\n`.
  - `distinct detail`: record nr 172 / EINVAL / `"prctl-option"` and nr 172 /
    EINVAL / `"prctl-set-vma"`. That gives two lines, each with `count=1`.
  - `unknown errno`: record nr 1 with error 200. The line contains `error=E200`.
  - `exe encoding`: exe `"/a b/%x"` formats as `exe=/a%20b/%25x`.
  - `overflow`: record 70 distinct numbers (1000–1069). That gives 64
    `linuxemu-unsupported` lines and one final line
    `linuxemu-unsupported-overflow exe=/bin/x pid=42 dropped=6\n`.
  - `short buffer`: with `capacity` 10, the return value is 0 and no partial
    line is written.
  - `reset`: after `linux_diag_reset()`, `linux_diag_format` returns 0.
- [ ] **Step 2: Run the test and confirm it fails.** Run the `cc` line above.
  Expected: a link error, because `linux_syscall_name` and friends are
  undefined.
- [ ] **Step 3:** Write `scripts/generate-syscall-names.sh` and run it with
  `ANDROID_NDK_HOME` set. It must emit 414 EABI entries plus the 6 ARM
  entries, sorted. Commit the generated header.
- [ ] **Step 4:** Implement the module. Lookups are a binary search over the
  sorted table. Format integers with a local decimal/hex writer, not
  `snprintf`.
- [ ] **Step 5: Run the test and confirm it passes.** Expected:
  `diagnostics_test_failures=0`.
- [ ] **Step 6:** Syntax-check `src/linux_diagnostics.c` against the QNX
  headers, using the same `clang -fsyntax-only` flags as before. Expected: no
  diagnostics.
- [ ] **Step 7: Commit.** Commit the script, the generated header, the
  module and the test.

### Task 2: Runtime glue (enable, record ENOSYS, flush on every exit path)

**Files:**
- Modify `src/linux_syscall.c`:
  - The dispatcher `default:` records `(number, LINUX_ENOSYS, 0, gpr[0..5])`.
  - `LINUX_NR_EXIT_GROUP` calls `linux_diag_flush()` before `_exit`.
  - The fork child path (next to `guest_thread_after_fork()`) calls
    `linux_diag_reset()`.
- Modify `src/guest_thread.c`: `guest_thread_exit` calls `linux_diag_flush()`
  before both `_exit` calls.
- Modify `src/trap.c`: `fatal_signal` calls `linux_diag_flush()` before
  `_exit(125)`.
- Modify `src/guest_process.c`:
  - Call `linux_diag_flush()` just before the host `execve`.
  - Forward `LINUXEMU_DIAGNOSTICS=<path>` into the new host environment,
    exactly as `LINUXEMU_SYSTRACE=1` is forwarded. Grow `host_environment`
    by one slot.
  - Forward `LINUXEMU_GUEST_EXE=<guest_executable>` the same way, and add
    `const char *guest_process_executable(void);`, which returns that value,
    or else the `loaded_path` given at start-up.
- Modify `src/linuxemu.c`: record the start-up path for
  `guest_process_executable()`, next to the existing
  `guest_process_initial_name` call.
- Create `src/linux_diagnostics_runtime.c` holding
  `void linux_diag_flush(void);`. Keeping it out of `linux_diagnostics.c`
  lets the pure module stay host-testable. It uses `getenv`, `getpid` and
  `guest_process_executable()`.
  - It does nothing when the variable is unset or the table is empty.
  - Otherwise it formats into a static 16 KiB buffer, then `open`
    (`O_WRONLY|O_CREAT|O_APPEND`, mode 0644), one `write`, `close`, and
    `linux_diag_reset()`.
  - It ignores every error.
- Create `guest-tests/diagnostics-syscalls.S`.
  - When `argc > 1`, it is the re-exec'd image: it calls nr 999 three times
    and exits 0.
  - Otherwise it calls nr 999 twice, then forks. The child calls nr 999
    once and exits 0. The parent waits, checks that the child exited 0, and
    then `execve`s itself (`argv[0]`) with the extra argument `child` and
    an empty environment. If `execve` returns, it exits 9.
- Add `diagnostics-syscalls` to the guest loop in
  `scripts/build-native-probes.sh`.
- Modify `scripts/run-linuxemu-smoke.sh`: add a `diagnostics` block that runs
  the cases in Step 1.
- Modify `README.md`: add a short "Diagnostics" section covering
  `LINUXEMU_DIAGNOSTICS=<file>`, the line format above, and that reports are
  appended per process.

**Interfaces:**
- Consumes: everything Task 1 produces.
- Produces:
  - `void linux_diag_flush(void);`
  - `const char *guest_process_executable(void);`
  - the report file format (Phase 0B/0C parse it).

- [ ] **Step 1: Write the failing smoke checks** in `run-linuxemu-smoke.sh`.
  Each uses a fresh file under `$project_dir/build/diag`:
  - `diag-basic`: run `diagnostics-syscalls` with `LINUXEMU_ROOT=/` (execve
    needs a guest root) and `LINUXEMU_DIAGNOSTICS=$f`. It exits 0, and the
    file has exactly three `nr=999 name=unknown error=ENOSYS` lines:
    - `count=2` with pid P (the parent before exec);
    - `count=1` with a pid other than P (the fork child); and
    - `count=3` with pid P (after exec, which proves the variable crosses
      `execve` and that the pre-exec counts were flushed, not carried
      over).
  - `diag-fatal`: run a tiny new guest `diagnostics-fatal.S` (nr 999 once,
    then `ldr r0, [r0]` with r0 = 0). It exits 125, and the file has the
    `nr=999` line.
  - `diag-off`: run `diagnostics-syscalls` without the variable. It exits 0,
    and `build/diag` holds no new file.
  - `diag-unwritable`: run with
    `LINUXEMU_DIAGNOSTICS=/nonexistent-dir/r.txt`. It still exits 0.
- [ ] **Step 2: Run the smoke suite on the Passport** with the old binary.
  Expected: `diag-basic` and `diag-fatal` fail (no file);
  `diag-off`/`diag-unwritable` pass.
- [ ] **Step 3:** Implement the glue listed under Files.
- [ ] **Step 4: Run the smoke suite.** Expected: `linuxemu_smoke_failures=0`.
- [ ] **Step 5: Stress check.** Run `thread-lifecycle-stress` and
  `futex-contention-stress` with `LINUXEMU_DIAGNOSTICS` set. Expected: same
  exit codes as without it, and no hang beyond the existing 10 s timeout.
- [ ] **Step 6: Run the full regression on the Passport.** Expected:
  `linuxemu_full_regression_failures=0`.
- [ ] **Step 7: Commit.**

### Task 3: Record the explicit unsupported-behaviour sites, and fix mmap2's host errno

**Files:**
- Modify `src/linux_syscall.c`. Add
  `static int32_t linux_unsupported(uint32_t number, int32_t linux_error, const char *detail, const uint32_t *args)`.
  It records the call and returns `-linux_error`. Use it at these sites, with
  these details:
  - `linux_prctl`: `PR_SET_VMA` → `"prctl-set-vma"`; unknown option →
    `"prctl-option"` (both EINVAL).
  - `linux_mmap2`: unknown flag bits → `"mmap-flags"` (EINVAL); shared +
    `PROT_EXEC` → `"mmap-shared-exec"`.
    **Fix:** it currently returns host `-EOPNOTSUPP` (QNX 103). Return
    `-LINUX_EOPNOTSUPP` (95).
  - `linux_eventfd2`: the non-wake form → `"eventfd-form"` (EOPNOTSUPP).
  - `linux_fcntl64`, and the `DUP`/`DUP2`/`DUP3` cases on a wake eventfd →
    `"wake-eventfd"` (EOPNOTSUPP).
  - The `utimensat` symlink case → `"utimensat-symlink"` (EOPNOTSUPP).
- Modify `guest-tests/memory-syscalls.S`: add a check that
  `mmap2(0, 0x1000, PROT_READ|PROT_EXEC, MAP_SHARED|MAP_ANONYMOUS, -1, 0)`
  returns `-95`. The new failure exits 60.

**Interfaces:**
- Consumes: `linux_diag_record` (Task 1).

- [ ] **Step 1: Write the failing tests.**
  - The `memory-syscalls.S` shared-exec check (exit 60).
  - `diag-prctl` in `run-linuxemu-smoke.sh`: run `android-process-syscalls`
    with `LINUXEMU_ROOT=/` and `LINUXEMU_DIAGNOSTICS=$f`. It exits 0, and
    the file has exactly two `nr=172 name=prctl error=EINVAL` lines:
    `count=1 detail=prctl-set-vma` and `count=1 detail=prctl-option`.
- [ ] **Step 2: Run them on the Passport.** Expected: `memory-syscalls` exits
  60, because the guest sees errno 103, and `diag-prctl` fails (no file).
- [ ] **Step 3:** Implement `linux_unsupported` and use it at the listed
  sites.
- [ ] **Step 4: Run the smoke suite.** Expected: `linuxemu_smoke_failures=0`.
- [ ] **Step 5: Run the full regression.** Expected:
  `linuxemu_full_regression_failures=0`.
- [ ] **Step 6:** Run the bionic probe with `LINUXEMU_DIAGNOSTICS` set. Paste
  the report lines into `docs/FINDINGS.md` as the first structured
  measurement.
- [ ] **Step 7: Commit.**
