#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
alpine_root=${ALPINE_ROOT:-"$project_dir/test-rootfs"}
linuxemu="$project_dir/build/linuxemu"
gcc="$alpine_root/usr/bin/gcc"
strip="$alpine_root/usr/bin/strip"
guest_source=/tmp/linuxemu-pthread-smoke.c
guest_binary=/tmp/linuxemu-pthread-smoke
fixture="$project_dir/guest-tests/fixtures/pthread-smoke-armhf"

if [ ! -x "$gcc" ] || [ ! -x "$strip" ]; then
    echo "guest gcc and strip are required; install Alpine build-base first" >&2
    exit 2
fi
cp "$project_dir/guest-tests/pthread-smoke.c" "$alpine_root$guest_source"
trap 'rm -f "$alpine_root$guest_source" "$alpine_root$guest_binary"' EXIT

attempt=1
while :; do
    if "$linuxemu" --linuxemu-exec "$alpine_root" / "$gcc" /usr/bin/gcc \
            -O2 -pthread -o "$guest_binary" "$guest_source"; then
        break
    fi
    if [ "$attempt" -ge 8 ]; then exit 1; fi
    attempt=$((attempt + 1))
done
LINUXEMU_ROOT="$alpine_root" "$linuxemu" "$strip" "$guest_binary"
mkdir -p "$(dirname -- "$fixture")"
cp "$alpine_root$guest_binary" "$fixture"
echo "wrote $fixture"
