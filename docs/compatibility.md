# Linuxemu v0.1 compatibility

Linuxemu runs 32-bit ARM Linux applications directly on the ARM CPU of a
BlackBerry 10 phone and translates their Linux system calls to QNX.

## Supported host

- BlackBerry 10 device with an ARMv7 CPU.
- The tested host reports QNX 8.0.0 and the BB10 10.3.1 target runtime.
- Ordinary Term49/SSH user execution; root is not required to run Linuxemu.
- BerryCore by sw7ft is required for the device installer, including its GNU
  Wget with HTTPS support and SHA-256 utilities.

## Supported guest

- Alpine Linux 3.24.2 armhf minirootfs, SHA-256
  `d86af88b58954d8a90c211004f1e61f208a62d1bb5a2f525f0c1cc26408ab92b`.
- Alpine musl 1.2.6 and BusyBox 1.37 from the pinned v3.24 repositories.
- Tested application packages include nano, OpenSSL-backed HTTPS tools,
  Python 3.14.7, SQLite, and Git 2.54.0.

## Verified behavior

The acceptance suite covers static and dynamic ELF loading, files and
directories, contained guest paths, memory mappings, terminals, clocks,
poll/select, IPv4/IPv6/UNIX sockets, DNS, verified HTTPS, apk transactions,
fork/exec/wait and `posix_spawn`, pthreads and the required futex operations,
Linux ARM signal frames and cancellation, runtime executable mappings,
Python application workflows, and local plus HTTPS Git workflows.

## Deliberate limits

- ARM `svc #0` patching is supported; general Thumb guest scanning is not.
- Priority-inheritance and robust-list futex operations are unsupported.
  Process-shared futexes do not synchronize separate Linuxemu host processes.
- Socket ancillary data, unlisted options, arbitrary ioctls, epoll, graphics,
  kernel modules, and Linux service management are outside the v0.1 surface.
- `eventfd2` implements the pipe-backed wake descriptor form needed by libcurl,
  rather than general Linux event counters.
- Executable shared file mappings are rejected. Private executable file
  mappings use an anonymous copy.
- Package installation is unprivileged and should use
  `apk add --no-chown PACKAGE`.
- Guest paths remain inside the Alpine rootfs. Host shared-media directories
  are not mounted into the guest.

Unsupported system calls return Linux errors. The supported release does not
promise compatibility with arbitrary Linux binaries beyond this tested matrix.
