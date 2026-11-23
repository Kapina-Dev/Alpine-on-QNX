#!/bin/sh
# Build the Phase 4 part 2 Thumb dlopen fixtures with the guest's own gcc:
# a Thumb-2 shared library and an ARM program that dlopens it. Neither is
# stripped: the library's mapping symbols are what Linuxemu patches by.
# ALPINE_ROOT must have Alpine build-base installed.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
gcc="$alpine_root/usr/bin/gcc"
fixture_dir="$project_dir/guest-tests/fixtures"

if [ ! -x "$gcc" ]; then
    echo "guest gcc is required; install Alpine build-base first" >&2
    exit 2
fi

for source in thumb-module.c thumb-dlopen.c; do
    cp "$project_dir/guest-tests/$source" "$alpine_root/tmp/$source"
done
trap 'rm -f "$alpine_root"/tmp/thumb-module.c "$alpine_root"/tmp/thumb-dlopen.c \
    "$alpine_root"/tmp/linuxemu-thumb-module.so \
    "$alpine_root"/tmp/linuxemu-thumb-dlopen' EXIT

"$linuxemu" --linuxemu-exec "$alpine_root" / "$gcc" /usr/bin/gcc \
    -O2 -fPIC -shared -march=armv7-a -mthumb -mfpu=vfp -mfloat-abi=hard \
    -o /tmp/linuxemu-thumb-module.so /tmp/thumb-module.c
"$linuxemu" --linuxemu-exec "$alpine_root" / "$gcc" /usr/bin/gcc \
    -O2 -marm -o /tmp/linuxemu-thumb-dlopen /tmp/thumb-dlopen.c

mkdir -p "$fixture_dir"
cp "$alpine_root/tmp/linuxemu-thumb-module.so" \
    "$fixture_dir/thumb-module-armhf.so"
cp "$alpine_root/tmp/linuxemu-thumb-dlopen" "$fixture_dir/thumb-dlopen-armhf"
echo "wrote Thumb fixtures under $fixture_dir"
