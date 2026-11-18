# Binder for Linuxemu — Approach and Plan

> The binder workstream of the Android track. It replaces the earlier
> "Option A first" binder plan in `docs/capability-roadmap.md` (Phase 9) and
> `ANDROID6_PLAN.md` (B2). Read `docs/FINDINGS.md` first. Keep external
> research labelled as external until reproduced on a device.

## Where this sits in the roadmap

- **B0** is not binder work. Its items belong to earlier phases: pid/tid
  virtualisation to Phase 6 (Android additions), and cross-process passing of
  emulator-managed fds to Phase 1's Android additions, finished in Phase 6.
- **B1–B3** (design document, read-only probes, disassembly study) are
  research. They don't change Linuxemu's code, so they can run before
  Phase 9, but only when the user schedules them; the phases stay in order.
- **B4–B5** are Phase 9 work. Their gate is the Phase 9 (android-m1) binder
  gate in `docs/capability-roadmap.md`.

## Context: Linuxemu is WSL1 for QNX

Linuxemu reimplements the Linux syscall and device surface so stock ARM32 Linux
binaries run natively on QNX. The Android goal is running a stock 32-bit Android
userland (6.0.1 first, 7.1.2 later) on top of it. WSL1 did not borrow a Linux kernel;
its `lxcore` driver reimplemented the syscalls and device semantics itself. Binder is
a device node, so it belongs in Linuxemu the same way epoll and eventfd do.

## Decision: write our own binder, behind a backend interface

### Why not reuse RIM's `/dev/binder` as the primary path

RIM's driver would be less code, but it is not reachable and carries costs:

1. **Not reachable.** As devuser, `open("/dev/binder")` returns EACCES. With a
   test-only `chmod 666`, `open` succeeds but devctl calls still return EACCES: the
   driver checks credentials itself. Only processes in RIM's Android player namespace
   pass, and both device attempts to enter that namespace failed (see FINDINGS,
   2026-11-17 spike). One of them broke the runtime.
2. **The context manager is taken.** RIM's Android 4.3 `servicemanager` owns the
   binder context on that driver. An Android 6 `servicemanager` cannot become context
   manager while it runs, and stopping it means controlling processes inside the
   player namespace.
3. **Non-trivial adapter.** External research reports packed WRITE_READ messages using
   offsets instead of pointers, CFG-created shared memory instead of `mmap`, and
   swapped ioctl direction bits.
4. **Every process looks like uid 1000.** RIM's driver would see all guest processes as
   the player uid, breaking Android's `getCallingUid()` checks unless we add a
   credential registry anyway.
5. **Not shippable.** It only works where RIM's runtime is installed and intact, and we
   cannot fix its bugs.

### The design

- Define a small **binder backend interface** inside Linuxemu: open, write_read,
  version, set_context_mgr, set_max_threads, receive-buffer mapping, release.
- **Backend 1 (build first): our own broker.** A Linuxemu-owned QNX process, in the
  spirit of Wine's `wineserver`, that holds binder state for all guest processes:
  nodes, refs, transactions, threads, death notifications. Guest `/dev/binder` is a
  virtual object in the fd layer; Linuxemu intercepts the guest's `ioctl` in-process
  and forwards it to the broker over a QNX channel (`ChannelCreate`/`MsgSend`) or
  `AF_UNIX`. **It must not use `resmgr_attach`**: that is gated by a BB10 ability that
  ordinary processes lack (stanw47 session79).
- **Backend 2 (optional, later): RIM's driver,** only if player-context access is ever
  solved. The guest-facing side (ioctl translation, pid virtualisation, fd passing) is
  shared by both backends, so none of the work is wasted.
- The same broker is the natural home for **ashmem** and the **property area** later.

## Reference material and how to use it

### AOSP binder driver (Linux kernel `drivers/android/binder.c`): the specification

This defines what stock Android 6 `libbinder` expects:
- every `BC_*` / `BR_*` command;
- transaction and reply rules, one-way calls;
- strong/weak reference counting;
- death notifications;
- the looper thread protocol (`BC_ENTER_LOOPER`, `BC_REGISTER_LOOPER`,
  `BR_SPAWN_LOOPER`);
- `BINDER_SET_CONTEXT_MGR`, `BINDER_SET_MAX_THREADS`, `BINDER_VERSION`;
- 32-bit protocol version 7 (`BINDER_IPC_32BIT`).

Use the kernel version matching the era (roughly 3.4–3.10, as used by Android 4.3–6).

### RIM's QNX binder: how binder maps onto QNX

Specimens in `stanw47/Blackberry-Classic-Research/specimens/`: `binder`, `libbionic.so`,
`libbinder.so`. Readability (checked 2026-11-18):

- **`binder` is a port of Linux `binder.c` into a QNX userspace resource manager.** Its
  log strings match the Linux source verbatim, e.g.
  `send failed reply for transaction %d to %d:%d`,
  `transaction release %d bad offset %d, size %d`,
  `%d:%d BC_ACQUIRE_DONE node %d has no pending acquire request`.
  Each string pins a disassembled function to the matching Linux function, so the
  binary can be read side by side with the source. Study what RIM *changed*: the QNX
  glue.
- **Small:** `.text` is about 16 KB (~4,000 ARM instructions). Stripped, no debug info.
- **Named exports:** `binder_devctl`, `binder_thread_write`, `binder_thread_read`,
  `binder_unblock`, `binder_transaction_log`, `binder_transaction_log_failed`.
- **QNX imports show the replacements for kernel facilities:**
  - messaging: `MsgSend`, `ChannelCreate`, `ConnectAttach`, `ConnectDetach`;
  - receive buffers: `shm_open`, `shm_ctl`, `shm_unlink`, **`mmap_peer` /
    `munmap_peer`** (maps memory into the *client's* address space, replacing Linux
    `mmap` on the device);
  - credentials: `iofunc_client_info_ext`, `isAndroidClient`, `getAndroidPlayerGid`,
    `retainBinderSystemCapabilities`;
  - blocking and threads: `thread_pool_*`, pthread mutex/condvar.
- **Client side is fully named:** RIM's `libbinder.so` exports 843 C++ symbols,
  including `IPCThreadState::talkWithDriver`, `transact`, `executeCommand` and
  `ProcessState::self`. Together with `libbionic.so`'s `ioctl_binder`, this shows the
  exact client-to-driver handshake.

**What to learn from RIM:**
- how transactions are carried over `MsgSend`/`MsgReply`;
- receive-buffer management with shared memory and peer mapping;
- the packed WRITE_READ message (write stream + read area in one message), a good fit
  for QNX message passing;
- blocking reads, thread pools and process-death handling on QNX.

**What not to take from RIM:**
- the resource-manager structure (blocked by the ability gate for us);
- the uid-1000-for-everyone credential model.

### Command words (external research; confirm before implementing)

| Operation | RIM command | Notes |
|---|---|---|
| WRITE_READ | `0xC0186201` | Confirmed as a literal in `libbionic.so` |
| VERSION | `0xC0046209` | nr 9, 4-byte payload (matches AOSP numbering) |
| CFG | `0xC108620C` | nr 12, 0x108-byte struct; *not* VERSION, despite an older note |
| TXN | `0xC03C620B` | 60 bytes |
| SET_MAX_THREADS | `0x80046205` | RIM direction bits differ from AOSP `0x40046205` |

Only WRITE_READ was found as a stored literal; the others appear to be built at
runtime. **Confirm every word by disassembling `ioctl_binder` (libbionic) and
`binder_devctl` (binder) before relying on it.** This only matters for Backend 2; the
broker implements the stock AOSP ioctl numbers the guest sends.

## Licensing

- Linux `binder.c` is GPLv2; RIM's binaries are proprietary; Linuxemu is MIT.
- **Read both to understand the design; write our own implementation. Copy no code.**
- Fine to use directly: the UAPI header (`include/uapi/linux/android/binder.h`) for
  structure layouts and command numbers, and Apache-licensed AOSP `libbinder` as the
  guest-side client.
- Never redistribute RIM binaries in this repo or in releases.
- `docs/rim-binder-notes.md` describes behaviour, message layouts and command
  words in our own words. It must not contain decompiled or disassembled RIM
  code: the publication audit (`docs/release.md`, step 6) forbids decompiled
  proprietary code in the repository history.

## Work plan

### B0. Prerequisites (from the larger plan; not binder work)
- PID/TID virtualisation (bionic requires pid/tid ≤ 65535; FINDINGS 2026-11-17).
  Binder also needs coherent small pids across the guest process set.
- A static bionic program reaching `main()`, then a dynamic one.
- fd passing between guest processes for emulator-managed objects (`SCM_RIGHTS`
  adoption beyond the current narrow scope), since binder passes fds.

### B1. Design document
Write `docs/binder-design.md` before coding. For each binder feature list:
1. AOSP semantics (from the Linux driver);
2. how RIM implemented it on QNX (from disassembly);
3. how Linuxemu's broker will implement it.

Cover: process attach/detach, nodes and refs, handle 0 / context manager, transactions
and replies, one-way queues, the receive buffer and its lifetime (`BC_FREE_BUFFER`),
fd and binder-object translation in transactions, looper threads, death notifications,
and error replies.

### B2. Probes (devuser, read-only, nothing in RIM's runtime touched)
- **Can an ordinary process use `mmap_peer` / `munmap_peer`?** This decides whether the
  broker can map receive buffers straight into guest processes, as RIM's driver does,
  or whether each guest maps a shared-memory object itself. Record the result in
  `FINDINGS.md`.
- Shared memory throughput between two devuser processes, to size buffers.

### B3. Disassembly study
- Load `binder`, `libbionic.so` (`ioctl_binder`) and `libbinder.so` in Ghidra (ARM
  32-bit LE). Cross-reference against the matching-era Linux `binder.c` using the log
  strings.
- Document findings in `docs/rim-binder-notes.md`: the QNX glue, the message layout,
  peer-mapping use, and the confirmed command words.

### B4. Broker implementation
- Backend interface in Linuxemu; guest `/dev/binder` as a virtual fd object.
- Broker process lifecycle: started on first `/dev/binder` open, survives guest
  processes, cleans up after crashed ones.
- Implement in this order, with a guest test for each:
  1. `BINDER_VERSION` returns 7;
  2. `BINDER_SET_MAX_THREADS`, `BINDER_SET_CONTEXT_MGR`;
  3. receive-buffer mapping (the guest's `mmap` on the binder fd);
  4. `BINDER_WRITE_READ` with `BC_ENTER_LOOPER` and an empty read that blocks and wakes;
  5. a transaction and reply between two guest processes;
  6. strong/weak refs and handle translation;
  7. one-way transactions;
  8. fd passing inside transactions;
  9. death notifications when a process exits;
  10. thread spawning (`BR_SPAWN_LOOPER`).

### B5. Milestones with stock AOSP 6.0.1 binaries
1. A minimal native client: `ProcessState::self()` → `defaultServiceManager()` →
   `checkService("nonexistent")` returns null cleanly.
2. AOSP `servicemanager` runs as context manager under Linuxemu.
3. A native test service registers and a client calls it.
4. AOSP's own binder tests (`binderDriverInterfaceTest`, `binderLibTest`) where they
   build for 6.0.1; otherwise equivalents.
5. Stress: many threads, many processes, process deaths mid-transaction, no leaks.

### Later
- Per-guest credentials (`getCallingUid`/`getCallingPid`) from Linuxemu's virtual
  pid/uid tables, so Android permission checks work.
- ashmem and the property area in the same broker.
- Optional Backend 2 (RIM's driver) if player-context access is ever proven.

## Rules
- Never modify RIM's runtime files. All RIM study is from the pulled specimens or
  read-only probes.
- Every binder feature gets a guest regression test that fails first.
- Log findings in `docs/FINDINGS.md` with date, device, result and evidence.
