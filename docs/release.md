# Release procedure

1. Build and validate Linuxemu on the supported device.
2. Run `ssh -tt bb10 'cd WORKSPACE && sh scripts/run-full-regression.sh'`.
3. Copy the validated `build/linuxemu` binary and full-regression summary to
   the release workspace.
4. Build all release assets and their checksum manifest with the final tagged
   GitHub release URL:

   ```sh
   LINUXEMU_BINARY=PATH sh scripts/prepare-release.sh \
     https://github.com/Kapina-Dev/Alpine-on-QNX/releases/download/v0.1.3
   ```

5. Test the rendered installer from an empty prefix on the device.
6. Verify the source tree and complete Git history contain no credentials,
   QNX SDK files, private device data, proprietary binaries, or decompiled
   proprietary code. Run `sh scripts/audit-publication.sh master` against the
   exact branch that will be pushed.
7. Publish the reviewed source privately, inspect it, and run the installer
   against the actual private release assets.
8. Make the repository public, create the annotated release tag, and publish
   the runtime bundle, its checksum, the rendered installer, compatibility
   document, and regression summary.

The QNX SDK, Alpine APK caches, test rootfs, device dumps, and recovery
artifacts are development prerequisites or evidence and are not release assets.

Push only the reviewed branch and explicit release tag. This working repository
contains local Codex checkpoint refs and unreachable development objects; never
publish it with `git push --mirror`.
