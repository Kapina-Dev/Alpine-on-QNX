# Source and release provenance

Linuxemu's tracked implementation, probes, and test programs were written as
part of this clean rebuild. The rebuild used behavior measured on the target
device and published Linux ABI definitions. It does not contain source or
decompiled code from BlackBerry, QNX, BerryCore, or the lost earlier Linuxemu
implementation.

The repository includes small ARM test executables built from the adjacent
tracked fixture sources. They exist only to make the device regression
reproducible. Their build scripts document the Alpine toolchain used to
regenerate them.

The following dependencies and development evidence are intentionally outside
the public repository and release bundle:

- the QNX SDK, device libraries, system binaries, logs, and filesystem dumps;
- private SSH keys and device-specific credentials;
- local recovery scripts, copied bytecode, and decompilation output;
- Alpine root filesystems, APK archives, and package caches; and
- BerryCore itself.

The release bundle contains only the Linuxemu executable, its project license,
version, and compatibility document. It dynamically uses the QNX libraries
already present on the phone. The installer uses the existing BerryCore tools
and downloads the pinned Alpine minirootfs directly from Alpine's official
server after verifying its recorded SHA-256 hash.

Only the named public branch is intended for publication. Local Codex
checkpoint refs and unreachable Git objects are development state and must not
be pushed. Run `sh scripts/audit-publication.sh master` and push the branch and
release tag explicitly; do not use `git push --mirror`.
