#!/bin/sh
# Build the static, ARM-mode bionic guest probe on a Linux host with the
# Android NDK (tested with r27c). Usage:
#   ANDROID_NDK_HOME=/path/to/android-ndk-r27c sh scripts/build-bionic-probe.sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
ndk=${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to an Android NDK}
bin="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
out=${OUT:-"$project_dir/build/guest-tests/bionic-probe-armv7"}

mkdir -p "$(dirname -- "$out")"
# Static builds need API >= 29's crtbegin_static.o: NDK r27's libc.a carries
# ELF TLS (gwp_asan), and only the API 29+ crt raises PT_TLS alignment to the
# 32 bytes bionic's static start-up requires on ARM. API 23's crt leaves it at
# 8 and bionic aborts ("executable's TLS segment is underaligned"). libc.a is
# the same for every API level, so this does not change the code under test.
"$bin/armv7a-linux-androideabi29-clang" -marm -static -O2 -Wall -Wextra \
    -o "$out" "$project_dir/guest-tests/bionic-probe.c"
"$bin/llvm-readelf" -h -l "$out" | grep -E 'Type:|Flags:|LOAD|INTERP' || true
sha256sum "$out"
