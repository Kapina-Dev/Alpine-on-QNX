#!/bin/sh
# Build the Phase 4 part 4 Android gate on the laptop: the two test
# programs (NDK r27c, Thumb, stripped) and a rootfs holding Android 6.0's
# /system/bin/linker and the 10 libraries the dynamic test loads, from
# build/android/system (scripts/fetch-android-sysimg.sh). Writes
# build/android/gate/ and build/android/android-gate.tar (git-ignored; the
# Google files are never committed). Upload the tar to the device's
# build/ and run scripts/run-android-gate.sh there.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
android="$project_dir/build/android"
system="$android/system"
ndk_bin="$android/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin"
gate="$android/gate"
libraries="libc.so libm.so libutils.so libdl.so libbacktrace.so
libcutils.so liblog.so libc++.so libbase.so libunwind.so"

for needed in "$system/bin/linker" "$ndk_bin/clang"; do
    if [ ! -e "$needed" ]; then
        echo "missing $needed: run scripts/fetch-android-sysimg.sh and" \
            "scripts/fetch-android-ndk.sh" >&2
        exit 2
    fi
done
rm -rf "$gate"
mkdir -p "$gate/system/bin" "$gate/system/lib" "$gate/data"

# Dynamic: PT_INTERP /system/bin/linker (the NDK default for 32-bit ARM),
# NEEDED libutils/libm/libc, linked against the image's own libraries.
"$ndk_bin/armv7a-linux-androideabi23-clang" -mthumb -O2 -Wall -Wextra \
    -Wl,--no-as-needed -Wl,--allow-shlib-undefined -L "$system/lib" \
    -o "$gate/data/android-gate" "$project_dir/guest-tests/android-gate.c" \
    -lutils -lm
# Static, own entry point: bionic's start-up never runs.
"$ndk_bin/armv7a-linux-androideabi29-clang" -mthumb -static -nostartfiles \
    -O2 -Wall -Wextra -o "$gate/data/android-syscall" \
    "$project_dir/guest-tests/android-syscall.c"
"$ndk_bin/llvm-strip" "$gate/data/android-gate" "$gate/data/android-syscall"

readelf="$ndk_bin/llvm-readelf"
"$readelf" -l "$gate/data/android-gate" |
    grep -q '\[Requesting program interpreter: /system/bin/linker\]' || {
    echo "android-gate: PT_INTERP is not /system/bin/linker" >&2; exit 1; }
for library in libutils.so libm.so libc.so; do
    "$readelf" -d "$gate/data/android-gate" |
        grep -q "Shared library: \[$library\]" || {
        echo "android-gate: no NEEDED $library" >&2; exit 1; }
done
if "$readelf" -S "$gate/data/android-syscall" | grep -q '\.symtab'; then
    echo "android-syscall: not stripped" >&2
    exit 1
fi

cp "$system/bin/linker" "$gate/system/bin/linker"
for library in $libraries; do
    cp "$system/lib/$library" "$gate/system/lib/$library"
done

# Expected patch sites per file, from the same decoder the core runs.
survey="$project_dir/build/code-map-survey"
gcc -std=gnu99 -O2 -I"$project_dir/src" -I"$project_dir/tests" -o "$survey" \
    "$project_dir/tests/code-map-survey.c" \
    "$project_dir/tests/code-map-compare.c" "$project_dir/src/code_map.c" \
    "$project_dir/src/thumb_decode.c"
"$survey" "$gate" | awk '$1 == "survey" {
    name = $2; sub("file=", "", name)
    sites = $NF; sub("sites=", "", sites)
    print name, sites }' | sort > "$gate/expected-sites.txt"
if awk '$2 == "-1" { bad = 1 } END { exit !bad }' "$gate/expected-sites.txt"
then
    echo "a gate file is refused by the decoder:" >&2
    grep ' -1$' "$gate/expected-sites.txt" >&2
    exit 1
fi

tar -cf "$android/android-gate.tar" -C "$gate" .
cat "$gate/expected-sites.txt"
echo "wrote $android/android-gate.tar"
