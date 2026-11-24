#!/bin/sh
# Build the Phase 4 part 3 stripped-decoding fixtures with the guest's own
# gcc: a mixed ARM/Thumb library kept unstripped (ground truth: mapping
# symbols) and stripped (decoded), a stripped Thumb library Linuxemu must
# refuse, and the program that compares and uses them.
# libgcc is linked statically: -funwind-tables code needs its unwind
# personality routines, and the test rootfs has no libgcc_s.
# ALPINE_ROOT must have Alpine build-base installed.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
fixture_dir="$project_dir/guest-tests/fixtures"
work=/tmp/linuxemu-stripped
flags="-O2 -fPIC -march=armv7-a -mfpu=vfp -mfloat-abi=hard -funwind-tables"

if [ ! -x "$alpine_root/usr/bin/gcc" ]; then
    echo "guest gcc is required; install Alpine build-base first" >&2
    exit 2
fi

guest() {
    "$linuxemu" --linuxemu-exec "$alpine_root" / "$alpine_root/usr/bin/$1" \
        "/usr/bin/$@"
}

rm -rf "$alpine_root$work"
mkdir -p "$alpine_root$work"
trap 'rm -rf "$alpine_root$work"' EXIT
for source in stripped-mixed-module.c stripped-mixed-arm.c \
    stripped-unknown-module.S stripped-compare.c; do
    cp "$project_dir/guest-tests/$source" "$alpine_root$work/$source"
done

guest gcc $flags -mthumb -c -o $work/mixed-thumb.o $work/stripped-mixed-module.c
guest gcc $flags -marm -c -o $work/mixed-arm.o $work/stripped-mixed-arm.c
guest gcc -shared -static-libgcc -o $work/mixed.so $work/mixed-thumb.o $work/mixed-arm.o
guest strip -o $work/mixed.stripped.so $work/mixed.so
guest gcc -shared -nostdlib -o $work/unknown.so \
    $work/stripped-unknown-module.S
guest strip $work/unknown.so
guest gcc -O2 -marm -o $work/compare $work/stripped-compare.c

mkdir -p "$fixture_dir"
cp "$alpine_root$work/mixed.so" "$fixture_dir/stripped-mixed-armhf.so"
cp "$alpine_root$work/mixed.stripped.so" \
    "$fixture_dir/stripped-mixed-armhf.stripped.so"
cp "$alpine_root$work/unknown.so" "$fixture_dir/stripped-unknown-armhf.so"
cp "$alpine_root$work/compare" "$fixture_dir/stripped-compare-armhf"
echo "wrote stripped-decoding fixtures under $fixture_dir"
