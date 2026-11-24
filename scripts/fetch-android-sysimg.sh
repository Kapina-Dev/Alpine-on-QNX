#!/bin/sh
# Fetch Google's API 23 armeabi-v7a emulator image and extract the files
# the Phase 4 part 4 Android gate needs: /system/bin, /system/lib and
# /system/build.prop. Run on the laptop (needs curl, unzip and debugfs from
# e2fsprogs). Google's binaries are never committed; OUT is under the
# git-ignored build/. The zip and the image are deleted once extracted.
#
#   sh scripts/fetch-android-sysimg.sh [OUT]   (default build/android/system)
#
# SHA-1 from Google's repository manifest
# (https://dl.google.com/android/repository/sys-img/android/sys-img2-1.xml).
# The image is Android 6.0 (build 3079352), not 6.0.1: Google published no
# 6.0.1 ARMv7 emulator image. Both are API 23.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
out=${1:-"$project_dir/build/android/system"}
url=https://dl.google.com/android/repository/sys-img/android/armeabi-v7a-23_r06.zip
expected=7cf2ad756e54a3acfd81064b63cb0cb9dff2798d
fingerprint='Android/sdk_phone_armv7/generic:6.0/MASTER/3079352:userdebug/test-keys'

if [ -f "$out/build.prop" ] &&
    grep -qxF "ro.build.fingerprint=$fingerprint" "$out/build.prop"; then
    echo "already extracted: $out"
    exit 0
fi
command -v debugfs >/dev/null 2>&1 || PATH=$PATH:/sbin:/usr/sbin
work="$out.download"
rm -rf "$work"
mkdir -p "$work" "$out"
trap 'rm -rf "$work"' EXIT
curl -fsSL -o "$work/sysimg.zip" "$url"
actual=$(sha1sum "$work/sysimg.zip" | awk '{print $1}')
if [ "$actual" != "$expected" ]; then
    echo "checksum mismatch: expected=$expected actual=$actual" >&2
    exit 1
fi
unzip -q -j "$work/sysimg.zip" armeabi-v7a/system.img -d "$work"
rm -f "$work/sysimg.zip"
# debugfs can't chown as an ordinary user; those messages are harmless.
debugfs -R "rdump /lib /bin /build.prop $out" "$work/system.img" 2>&1 |
    grep -v 'Operation not permitted while changing ownership' || true
if ! grep -qxF "ro.build.fingerprint=$fingerprint" "$out/build.prop"; then
    echo "extraction failed: $out/build.prop has the wrong fingerprint" >&2
    exit 1
fi
echo "extracted $out"
