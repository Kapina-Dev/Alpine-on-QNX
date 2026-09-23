#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
gcc="$alpine_root/usr/bin/gcc"
strip="$alpine_root/usr/bin/strip"
fixture_dir="$project_dir/guest-tests/fixtures"

if [ ! -x "$gcc" ] || [ ! -x "$strip" ]; then
    echo "guest gcc and strip are required; install Alpine build-base first" >&2
    exit 2
fi

for source in dlopen-module.c dlopen-stress.c spawn-thread-stress.c; do
    cp "$project_dir/guest-tests/$source" "$alpine_root/tmp/$source"
done
trap 'rm -f "$alpine_root"/tmp/dlopen-module.c \
    "$alpine_root"/tmp/dlopen-stress.c \
    "$alpine_root"/tmp/spawn-thread-stress.c \
    "$alpine_root"/tmp/linuxemu-phase9-module.so \
    "$alpine_root"/tmp/linuxemu-dlopen-stress \
    "$alpine_root"/tmp/linuxemu-spawn-thread-stress' EXIT

"$linuxemu" --linuxemu-exec "$alpine_root" / "$gcc" /usr/bin/gcc \
    -O2 -fPIC -shared -o /tmp/linuxemu-phase9-module.so \
    /tmp/dlopen-module.c
"$linuxemu" --linuxemu-exec "$alpine_root" / "$gcc" /usr/bin/gcc \
    -O2 -o /tmp/linuxemu-dlopen-stress /tmp/dlopen-stress.c -ldl
"$linuxemu" --linuxemu-exec "$alpine_root" / "$gcc" /usr/bin/gcc \
    -O2 -pthread -o /tmp/linuxemu-spawn-thread-stress \
    /tmp/spawn-thread-stress.c

for binary in linuxemu-phase9-module.so linuxemu-dlopen-stress \
    linuxemu-spawn-thread-stress; do
    "$linuxemu" --linuxemu-exec "$alpine_root" / "$strip" /usr/bin/strip \
        "/tmp/$binary"
done

mkdir -p "$fixture_dir"
cp "$alpine_root/tmp/linuxemu-phase9-module.so" \
    "$fixture_dir/phase9-module-armhf.so"
cp "$alpine_root/tmp/linuxemu-dlopen-stress" \
    "$fixture_dir/dlopen-stress-armhf"
cp "$alpine_root/tmp/linuxemu-spawn-thread-stress" \
    "$fixture_dir/spawn-thread-stress-armhf"
echo "wrote Phase 9 fixtures under $fixture_dir"
