# Linuxemu

**Run Alpine Linux apps on a BlackBerry 10 phone.**

Linuxemu runs unmodified 32-bit ARM Linux programs on BlackBerry 10. There is
no virtual machine and no CPU emulation: the phone's CPU runs the Linux code
directly, and Linuxemu translates each Linux system call into its QNX
equivalent. It works like WSL1 on Windows, but for QNX.

You get an Alpine Linux 3.24 shell with `apk`, so you can install and run
Python, Git, Vim, GCC, tmux and more on the phone, without root and without
flashing anything. It's developed and tested on a Passport.

```sh
alpinx                          # open the Alpine shell
apk add --no-chown python3 git  # then install packages as usual
uname -m                        # armv7l: Linux code, running natively
```

## What works

These apps are installed from Alpine's own packages and scored on install,
start-up, a basic workload, a concurrent workload, and clean shutdown:

| App | Status | Notes |
| --- | --- | --- |
| BusyBox | ✅ works | |
| apk | ✅ works | Use `apk add --no-chown` |
| Python 3.14 | ✅ works | Threads, sockets, HTTPS, SQLite, subprocesses |
| Git 2.54 | ✅ works | Local repositories and HTTPS clone, fetch and push |
| SQLite | ✅ works | |
| Vim | ✅ works | |
| GCC and make | ✅ works | Compiles and runs programs on the phone |
| inotify-tools | ✅ works | `inotify` on QNX's native one; events arrive about 250 ms late (limits in [compatibility.md](docs/compatibility.md)) |
| Redis | ✅ works | |
| nginx | ❌ not yet | Starts, but no worker: `ioctl(FIOASYNC)` on its worker channel returns `ENOTTY` (async I/O signals, `FIOASYNC`/`F_SETOWN`, aren't supported yet) |
| Node.js | ❌ not yet | Stops at start-up |

tmux 3.7 also works, attached and detached, in Term49 and over SSH. All
results come from runs on a real Passport. See
[corpus-results.md](docs/corpus-results.md) for the per-stage results and
[compatibility.md](docs/compatibility.md) for the supported surface.

## Install

You need a BlackBerry 10 phone with BerryCore by sw7ft installed. Run these as the ordinary Term49 or SSH user:

```sh
wget -O /tmp/install-linuxemu.sh https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.6/install-linuxemu.sh
sh /tmp/install-linuxemu.sh
. ~/.profile
alpinx
```

The installer checks the SHA-256 of the Linuxemu runtime and of the official
Alpine 3.24.2 armhf minirootfs. It then installs and smoke-tests everything
before it replaces an existing installation.

## Use

| Command | What it does |
| --- | --- |
| `alpinx` or `linuxemu` | Opens an Alpine login shell (Bash) |
| `linuxemu COMMAND ARG...` | Runs one Linux command |
| `linuxemu -c 'SHELL COMMAND'` | Runs a shell command line |
| `linuxemu-rollback` | Switches back to the previous installation |
| `linuxemu-uninstall` | Removes Linuxemu |

Install packages with `apk add --no-chown PACKAGE`. Linux programs only see
the Alpine root filesystem, not the rest of the phone. DNS settings,
custom install locations and other options are covered in
[installation.md](docs/installation.md).

## How it works

1. Linuxemu loads the Linux ELF executable, and musl's dynamic loader when
   the program needs it, into the memory of an ordinary QNX process.
2. Before the code runs, every Linux system-call instruction (`svc #0`) is
   rewritten into a trap. TLS register reads are rewritten the same way,
   because QNX does not keep that register per thread.
3. When the program traps, Linuxemu translates the call into QNX: arguments,
   flags, structure layouts, error numbers and signals. Then it resumes the
   program.

Everything else runs at full native speed. Paths are kept inside the guest
root filesystem, and Linuxemu gives the guest its own descriptor table,
signal frames, futexes and PTYs, since QNX's versions behave differently from
Linux's.

## Limits

- Only 32-bit ARM Linux (armhf). It's tested with Alpine 3.24 (musl) and
  QNX 8.0.0 / BB10 10.3.
- Not a Linux kernel. There are no graphics, kernel modules or systemd,
  and calls Linuxemu doesn't support return Linux errors.
- Default-speed `cmatrix` freezes the Term49 display. Use
  `cmatrix -s -u 10`.

The full list is in [compatibility.md](docs/compatibility.md).

## Roadmap

[capability-roadmap.md](docs/capability-roadmap.md) sets out the next phases:
contained modern syscalls such as `pipe2`, `dup3` and full `eventfd`, then
`epoll`, `timerfd`, `signalfd` and `inotify` (all landed; Redis and
inotify-tools now run), and next the gaps the corpus still shows, such as
nginx's `FIOASYNC` and Node.js's start-up crash.

## Development

Building on the device, the 14 regression suites, the
`LINUXEMU_DIAGNOSTICS` log, the application corpus and the source layout are
described in [development.md](docs/development.md). The release process is
in [release.md](docs/release.md), and [FINDINGS.md](docs/FINDINGS.md) records
what the device has taught us about QNX.

## Thanks

Respect and thanks to **sw7ft** for BerryBrowser, BerryCore, and his wider
efforts to keep BlackBerry devices alive and useful. Linuxemu builds on that
same spirit of extending the practical life of these devices.

## License

[MIT](LICENSE)
