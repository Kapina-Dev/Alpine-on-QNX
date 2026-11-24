# Linuxemu → Android 6 (API 23) on BB10/QNX — Implementation Plan

> **Superseded (2026-11-16):** this plan has been merged into
> `docs/capability-roadmap.md` (Android additions in Phases 0–7, and Phases
> 9–11). Edit the roadmap, not this file. This copy is kept for history only.

> For Claude Code working in the Linuxemu repo
> (https://github.com/Kapina-Dev/Alpine-on-QNX). Read this file **and**
> `docs/capability-roadmap.md` before starting.
>
> **Part A** follows Linuxemu's own capability roadmap. It does not replace it. It adds
> the Android-specific acceptance tests to each roadmap phase.
> **Part B** adds the Android layers on top, and starts only after Part A passes.
> Never start a phase before the previous phase's exit criteria pass.

## Goal

Run unmodified **WhatsApp** and **Viber** APKs on BlackBerry 10 phones (Passport first,
then Classic) by booting a stock **32-bit ARM Android 6.0.1 (API 23)** userland as Linux
guest processes under Linuxemu. QNX stays as the kernel. Nothing is flashed. The end
result is a native BB10 launcher BAR that opens a full Android 6 UI in a QNX Screen window.

Why API 23: as of Nov 2026, both apps require Android 6.0+ (WhatsApp since 2026-09-08,
Viber per its help page). That's the floor today, and it will be raised again, probably
within 1–3 years.

## Where Linuxemu stands (v0.1.5, last commit 2026-10-02)

- ~6.3k lines of C in `src/`.
- Works: ARM-mode `svc` patching, ELF/musl loading, signals and queued signals,
  pthreads, futexes (single process), fork/clone/execve/wait4, termios, sockets,
  HTTPS, apk, Python 3.14 (including a threaded HTTP server after the self-stack
  teardown fix), Git.
- Partial: `eventfd2` is a pipe-backed wake-descriptor subset only (the libcurl case).
  Every other flag combination returns `EOPNOTSUPP`.
- Missing: virtual fd layer, epoll, full eventfd, timerfd, signalfd, inotify, Thumb,
  runtime-mapping patching, synthetic `/proc`, process-shared futexes, robust lists.

## Hard rules

- Follow the roadmap's design principles:
  - implement Linux semantics explicitly
  - return correct Linux errors, with **no success stubs**
  - one test for every fix
  - keep ordinary-user execution as the supported environment
  - keep Bash as the default shell
- **Never redistribute RIM binaries.** You may study them, and at runtime you may call
  the copies already on the user's device.
- No flashing, no boot-partition writes, nothing irreversible on the device.
- Develop on the **Passport** (3 GB RAM, 4× Krait 400, Adreno 330). Keep the
  **Classic** (2 GB, 2 cores, 720×720) working.
- Log every finding in `docs/FINDINGS.md`: date, device, what was tried, result, evidence.
  When a hypothesis here is wrong, record it and adjust the plan.
- When blocked by a BB10 permission or ability gate, write up the exact failing call
  and errno before trying workarounds.

## Reference: RIM's Android 4.3 runtime (study specimen)

Repo: https://github.com/stanw47/Blackberry-Classic-Research

- RIM **recompiled** Android 4.3 for QNX: `libbionic.so` sits on QNX `libc.so.3`. We
  don't recompile anything; we run stock Linux binaries and translate underneath.
  RIM's code is a guide for how each Linux behaviour maps onto QNX.
- `notes/session35-binder-over-qnx-deciphered.md`: binder is a userland QNX resmgr
  at `/dev/binder`. `libbionic.ioctl_binder` forwards `BINDER_WRITE_READ` (0xc0186201)
  and `BINDER_VERSION` (0xc108620c) via QNX `ioctl` + `MsgSendv_r`, with shm payloads.
- `notes/session36-rim-port-architecture-map.md`: maps each AOSP subsystem to QNX:
  - properties → PPS, logging → slog2
  - futex → `SyncCondvar*`, epoll → `poll`/`ionotify`
  - graphics → libscreen, audio → ALSA, camera → camapi
- `binder/notes-session79-resmgr-ability-gate.md`: **`resmgr_attach` returns EPERM for
  non-system processes anywhere on BB10.** Be a client of existing resmgrs, or use
  brokers that don't call `resmgr_attach`.
- `specimens/epolld` is a resmgr at `/dev/android/epoll` using `iofunc_notify*` and
  `MsgDeliverEvent`. `specimens/libbionic.so` exports `epoll_*`,
  `_epoll_notify_close`, `eventfd*`, `futex`, `__system_property_*` and `ioctl_binder`.
- `notes/ANDROID-APK-TO-DRAWER-FLOW.md`: BB10's own APK install and registry flow.
  Ours bypasses it.

---

# PART A — Linuxemu roadmap, with Android acceptance tests added

Do the roadmap phases in the roadmap's order. Below, each phase lists only what
Android 6 additionally needs. A phase is done when the roadmap's gate **and** these
extra checks pass.

## A0. Roadmap Phase 0 — measurable compatibility
Implement as specified: structured unsupported-syscall diagnostics, plus the
application corpus.

Android additions:
- Add an **"android-probe" corpus entry**: run selected AOSP 6.0.1 armv7 binaries
  (`toybox`/`toolbox`, `logwrapper`, `servicemanager --help`-style startup, `linker`
  loading a trivial bionic program) under Linuxemu with the diagnostics on.
- Commit the unsupported-syscall report as `docs/android-syscall-gap.md`. It is the
  measured to-do list for Part A, and should be updated after every phase.
- Note: bionic's linker is at `/system/bin/linker` and expects `/system/lib`. Give the
  probe its own rootfs so it doesn't disturb Alpine.

## A1. Roadmap Phase 1 — virtual file-descriptor layer
Implement as specified. Everything in A3 and B2 depends on this layer.

Android additions — the layer's object model must also handle (design these in now,
implement them in Part B):
- `ashmem` regions: an fd backed by a QNX shm object, with size, name and prot mask.
- binder handles: an fd representing a connection to the binder backend, with a
  per-process mmap'd receive buffer.
- **fd passing between guest processes.** Binder and `SCM_RIGHTS` both move fds across
  processes, so emulator-managed objects need a way to be shared across processes,
  not just within one.
- Gate: a test that sends an eventfd and a native pipe over an `AF_UNIX` socket with
  `SCM_RIGHTS` to a forked child, and both still work there.

## A2. Roadmap Phase 2 — contained modern syscalls
Implement as specified (`pipe2`/`dup3` flags, `accept4`, `preadv`/`pwritev`,
`fallocate`, `sendmmsg`/`recvmmsg`, `fcntl64`, rlimits).

Android additions:
- `prctl`: `PR_SET_NAME`, `PR_GET_NAME`, `PR_SET_DUMPABLE`, `PR_SET_NO_NEW_PRIVS`,
  `PR_SET_PDEATHSIG`. Implement real semantics where possible, and return proper
  errors otherwise.
- `setpriority`/`getpriority`, `sched_setscheduler`/`sched_getscheduler`,
  `sched_setaffinity` with correct errors.
- `capget`/`capset`, at minimum reporting a consistent unprivileged capability set.
  Zygote and installd query these.
- `personality`, `getrlimit`/`setrlimit` for `RLIMIT_NOFILE` and `RLIMIT_STACK`.

## A3. Roadmap Phase 3 — Linux event interfaces
Implement as specified: full eventfd, epoll (native `poll()` backend), timerfd,
signalfd, inotify.

Android additions:
- **Probe RIM's `epolld` first** (research only, before writing epoll):
  1. With the 4.3 runtime running, a QNX-native `native-probes/probe_epolld.c`
     records owner, group and mode of `/dev/android/epoll` and `/dev/binder`,
     opens `/dev/android/epoll`, and registers a pipe.
  2. Run it as the ordinary user, and as root if available.
  3. Disassemble `libbionic.so`'s `epoll_*` / `_epoll_notify_close` to recover the
     devctl protocol, and document it in `docs/rim-epolld-protocol.md`.
  4. Results go in `FINDINGS.md`. Even if `epolld` turns out usable, the native backend
     is still the default. A RIM backend is optional (`LINUXEMU_EPOLL=rim`) and only
     works with the 4.3 runtime installed.
- Android's `Looper` pattern must pass: one epoll fd watching an eventfd (wake) plus
  several pipes and sockets, level-triggered, `epoll_wait` with timeouts, wakes from
  another thread, and fds added and removed while another thread is waiting.
- timerfd with `CLOCK_BOOTTIME` and `CLOCK_REALTIME_ALARM`/`CLOCK_BOOTTIME_ALARM`.
  For the ALARM clocks, return `EINVAL` or map them deliberately, and document which.
  AlarmManager probes these.
- inotify must cover `IN_CREATE`, `IN_DELETE`, `IN_MOVED_*`, `IN_CLOSE_WRITE` on
  directories. That's FileObserver's and PackageManager's usage.

## A4. Roadmap Phase 4 — Thumb and stripped binaries
Implement as specified.

Android additions:
- **Patching code mapped at runtime is mandatory for Android.** Bionic's linker loads
  every library with `mmap`/`mprotect` itself, and ART maps `.oat` files with
  `PROT_EXEC`.
- Stripped binaries matter: most AOSP `/system/lib` and APK native libraries are
  stripped of section headers. The roadmap's "validated executable-segment decoding"
  step is required, not optional.
- Gate: AOSP 6.0.1 `libc.so`/`libm.so`/`libutils.so` (mostly Thumb-2, stripped) load
  through bionic's linker, and a Thumb NDK test binary's raw `syscall()` calls are
  intercepted.
- Decision to record in `FINDINGS.md`: whether Part B also ships a **modified bionic**
  whose syscall stubs call the translator directly (belt and braces), or relies on
  patching alone. **Decided 2026-11-24: patching alone** (`FINDINGS.md`, Phase 4
  part 4).

## A5. Roadmap Phase 5 — synthetic procfs
Implement as specified.

Android additions:
- `/proc/self/maps` must be exact. ART parses it, and so do debuggerd and libunwind.
- Add `/proc/<pid>/stat`, `/proc/<pid>/cmdline`, `/proc/<pid>/oom_score_adj`
  (writable, stored per process), `/proc/sys/kernel/pid_max`,
  `/proc/sys/vm/overcommit_memory`.
- `/sys`: only what a measured workload asks for. Expect
  `/sys/devices/system/cpu/possible` and `online`.

## A6. Roadmap Phase 6 — process and synchronization depth
Implement as specified.

Android additions (these become required, not "where achievable"):
- **Process-shared futexes across fork** on `MAP_SHARED` memory. The property area
  and its waits depend on this.
- Robust futex lists (`set_robust_list`). Bionic registers them for pthread mutexes.
- `vfork` and `clone(CLONE_VM|CLONE_VFORK)` behaving correctly enough for bionic's
  `posix_spawn` and `system()`.
- **Fork stress like zygote's:** a parent with ~30 threads, many mappings and open fds
  forks 100 children in a row, and each child execs or runs and exits cleanly with no
  fd or mapping leaks.

### Part A exit criteria
- The roadmap's Phase 0–6 gates pass on the Passport.
- `docs/android-syscall-gap.md` shows no unsupported syscalls for the android-probe
  corpus, apart from the device-file ioctls handled in Part B.

---

# PART B — Android 6 layers

## B1. System image
- Get a 32-bit ARM AOSP **android-6.0.1** userland, either a generic armv7 build or
  your own AOSP build. Write the recipe in `android/BUILD.md`, with checksums.
- Guest rootfs layout: `/system`, `/data`, `/cache`, `/dev`, `/proc`, `/sys`, separate
  from Alpine. Map a BB10 shared folder (e.g. `/accounts/1000/shared/downloads`) to
  guest `/sdcard`.
- SELinux disabled or permissive. Trimmed `init.rc`.

## B2. Fake Android device files (as virtual fd objects from A1)
- `/dev/ashmem`: `ASHMEM_SET_NAME`, `SET_SIZE`, `GET_SIZE`, `SET_PROT_MASK`,
  `GET_PROT_MASK`, `PIN`, `UNPIN`, `GET_PIN_STATUS`, `PURGE_ALL_CACHES`. Backed by QNX
  `shm_open`. Must survive fork and fd passing.
- **The property area:** guest `/dev/__properties__`, a shared region mapped into every
  guest process. Property waits use the A6 shared futexes. Writes go through init's
  `property_service` socket as normal.
- **`/dev/binder`**, 32-bit protocol version 7. Follow `docs/binder-plan.md`
  (decided 2026-11-18), which replaces the earlier "RIM's driver first" order:
  - **Backend 1 (build first):** our own broker that **does not call
    `resmgr_attach`**. It's a Linuxemu-owned QNX process reached through
    `ChannelCreate`/`MsgSend`, or `AF_UNIX`, that implements:
    - `BC_*`/`BR_*` command streams, transactions and replies, one-way calls
    - strong and weak reference counting, death notifications
    - passing fds and binder objects
    - a per-process mmap'd receive buffer backed by shm
    - `BINDER_SET_CONTEXT_MGR`, `BINDER_SET_MAX_THREADS`, the looper thread protocol
  - **Backend 2 (optional, later):** forward to RIM's `/dev/binder` the way
    `libbionic.ioctl_binder` does, only if player-context access is ever solved.
    As devuser the driver refuses access, and its context manager is RIM's.
  - Test against AOSP's own `binderLibTest`/`binderDriverInterfaceTest` where they
    build for 6.0.1. Otherwise write equivalents.
- AlarmManager: Android 6 uses `/dev/alarm` if it's present and timerfd otherwise.
  Prefer providing the timerfd path. Also provide `/dev/null`, `/dev/zero`,
  `/dev/urandom`, `/dev/random`, `/dev/tty`, `/dev/ptmx` + `/dev/pts`.

## B3. Boot without a screen — milestones in order
Stub HALs: audio, sensors, radio/RIL, camera, lights, power, GPS.
1. **`servicemanager` + a native binder client/server test.** This is the key milestone.
2. `init` parses `init.rc`, starts services, and `property_service` works.
3. `logd` runs and `logcat` shows output.
4. Zygote (`app_process -Xzygote /system/bin --zygote --start-system-server`) preloads
   classes and forks.
5. `system_server` stays up for more than 10 minutes with no crash loop.
6. `adbd` runs, and `adb shell pm list packages` works from a PC.
7. `dex2oat` compiles a test APK during `adb install`, and the app's activity starts
   (verified headlessly with `am start` + logcat).

Exit: milestones 1–7 pass on the Passport, with logs in `FINDINGS.md`.

## B4. Display and input
- **Software rendering first:** a gralloc + framebuffer/hwcomposer HAL that copies
  SurfaceFlinger's final frame into one QNX Screen window. Use Android's software GLES
  (`libagl`).
  - Render at 720×720 and let Screen scale to 1440×1440 on the Passport.
  - RIM's `libframebuffer_screen.so`, `libhwcwindow.so` and `libgralloc_screen.so`
    imports show which `screen_*` calls work. These parts are QNX-side, so they
    belong in the host/bridge, not the guest.
- **Input:** QNX Screen touch and keyboard events → synthetic evdev devices
  (`/dev/input/event*`, with `EVIOCG*` ioctls) for InputReader. Mapping the Passport
  keyboard's touch gestures is optional.

## B5. Launcher BAR and apps
- A native BB10 app that:
  - stops RIM's 4.3 runtime if needed (`stop-android.sh`), which is required on the Classic
  - boots Android 6 under Linuxemu and shows the Screen window
  - restarts the 4.3 runtime when it exits
- **WhatsApp:**
  - First confirm the current APK still contains `lib/armeabi-v7a`.
  - `adb install`, register with the SMS or voice code typed in by hand.
  - Verify sending and receiving messages and in-container notifications.
- **Viber:** same checks. It may need microG for FCM push.

Exit: WhatsApp sends and receives messages on the Passport, then on the Classic, with
performance notes.

## Later (out of scope until B5 passes)
- Hardware GLES through QNX EGL (risky: bionic and QNX libc ABI/TLS clash).
- One Screen window per Android window. RIM's `libhwcwindow` is the model.
- Per-app launcher BARs (`am start -n <pkg>/<activity>`) so apps get drawer icons.
- Notifications forwarded to the BlackBerry Hub through PPS. Audio and camera for calls.

## Optional side quest (only if the user asks)
A whatsmeow-based WhatsApp terminal client under Alpine, as soon as A3's epoll lands
(Go's netpoller needs it).
- It breaks WhatsApp's ToS, so the number risks a ban.
- It needs a primary phone that comes online periodically.

## Working conventions
- Small commits, one feature each, with a matching regression test.
- Keep the existing v0.1.x release regression passing on every commit.
- Update `docs/android-syscall-gap.md` and `docs/FINDINGS.md` as you go.
